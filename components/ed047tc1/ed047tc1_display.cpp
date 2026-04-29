#include "ed047tc1_display.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

extern "C" {
#include "ed047tc1.h"
#include "i2s_data_bus.h"
#include "rmt_pulse.h"
}

// Rect_t is normally from epd_driver.h which we don't include
typedef struct {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} Rect_t;

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <string.h>

namespace esphome {
namespace ed047tc1 {

static const char *const TAG = "ed047tc1";

// ---- Grayscale rendering extracted from vendor epd_driver.c ----

#define EPD_LINE_BYTES (EPD_WIDTH / 4)
#define CLEAR_BYTE     0b10101010
#define DARK_BYTE      0b01010101

static const int32_t contrast_cycles_4[15] = {30, 30, 20, 20, 30, 30, 30, 40, 40, 50, 50, 50, 100, 200, 300};

static uint8_t *conversion_lut = nullptr;
static QueueHandle_t output_queue = nullptr;
static uint32_t skipping = 0;

typedef struct {
    uint8_t *data_ptr;
    SemaphoreHandle_t done_smphr;
    Rect_t area;
    int32_t frame;
} OutputParams;

static void IRAM_ATTR reset_lut(uint8_t *lut_mem) {
    memset(lut_mem, 0x55, (1 << 16));
}

static void IRAM_ATTR update_lut(uint8_t *lut_mem, uint8_t k) {
    // This is dark magic lifted wholesale from the example code,
    // my vague LLM driven understanding is that each screen update
    // is actually 15 successive updates. e-paper works by poking
    // the pixels and exciting the particles in it, the more you
    // poke them the more they'll go dark, so you get grayscale by
    // poking darker pixels more.
    uint8_t kk = 15 - k;

    for (uint32_t l = kk; l < (1 << 16); l += 16)
        lut_mem[l] = (lut_mem[l] & 0xFC) | 0x02;
    for (uint32_t l = (kk << 4); l < (1 << 16); l += (1 << 8))
        for (uint32_t p = 0; p < 16; p++)
            lut_mem[l + p] = (lut_mem[l + p] & 0xF3) | 0x08;
    for (uint32_t l = (kk << 8); l < (1 << 16); l += (1 << 12))
        for (uint32_t p = 0; p < (1 << 8); p++)
            lut_mem[l + p] = (lut_mem[l + p] & 0xCF) | 0x20;
    for (uint32_t p = (kk << 12); p < ((kk + 1) << 12); p++)
        lut_mem[p] = (lut_mem[p] & 0x3F) | 0x80;
}

static void IRAM_ATTR calc_epd_input_4bpp(uint32_t *line_data, uint8_t *epd_input, uint8_t *lut) {
    uint32_t *wide_epd_input = (uint32_t *)epd_input;
    uint16_t *line_data_16 = (uint16_t *)line_data;
    for (uint32_t j = 0; j < EPD_WIDTH / 16; j++) {
        uint16_t v1 = *(line_data_16++);
        uint16_t v2 = *(line_data_16++);
        uint16_t v3 = *(line_data_16++);
        uint16_t v4 = *(line_data_16++);
        // USER_I2S_REG=0 path (esp_lcd)
        wide_epd_input[j] = (lut[v1] << 0) | (lut[v2] << 8) | (lut[v3] << 16) | (lut[v4] << 24);
    }
}

static void IRAM_ATTR nibble_shift_buffer_right(uint8_t *buf, uint32_t len) {
    uint8_t carry = 0xF;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t val = buf[i];
        buf[i] = (val << 4) | carry;
        carry = (val & 0xF0) >> 4;
    }
}

static void write_row(uint32_t output_time_dus) {
    skipping = 0;
    epd_output_row(output_time_dus);
}

static void skip_row(uint8_t pipeline_finish_time) {
    if (skipping == 0) {
        epd_switch_buffer();
        memset(epd_get_current_buffer(), 0, EPD_LINE_BYTES);
        epd_switch_buffer();
        memset(epd_get_current_buffer(), 0, EPD_LINE_BYTES);
        epd_output_row(pipeline_finish_time);
    } else if (skipping < 2) {
        epd_output_row(10);
    } else {
        epd_skip();
    }
    skipping++;
}

static void reorder_line_buffer(uint32_t *line_data) {
    for (uint32_t i = 0; i < EPD_LINE_BYTES / 4; i++) {
        uint32_t val = *line_data;
        *(line_data++) = val >> 16 | ((val & 0x0000FFFF) << 16);
    }
}

static void IRAM_ATTR provide_out(OutputParams *params) {
    uint8_t line[EPD_WIDTH / 2];
    memset(line, 255, EPD_WIDTH / 2);
    Rect_t area = params->area;
    uint8_t *data = params->data_ptr;

    if (params->frame == 0)
        reset_lut(conversion_lut);
    update_lut(conversion_lut, params->frame);

    for (int32_t i = 0; i < EPD_HEIGHT; i++) {
        if (i < area.y || i >= area.y + area.height)
            continue;

        uint8_t *row_ptr = data + i * (EPD_WIDTH / 2);

        uint32_t *lp;
        bool shifted = false;
        if (area.width == EPD_WIDTH && area.x == 0) {
            lp = (uint32_t *)row_ptr;
        } else {
            uint8_t *buf_start = line;
            uint32_t line_bytes = area.width / 2 + area.width % 2;
            if (area.x >= 0) {
                buf_start += area.x / 2;
            } else {
                line_bytes += area.x / 2;
            }
            uint32_t max_bytes = EPD_WIDTH / 2 - (uint32_t)(buf_start - line);
            if (line_bytes > max_bytes) line_bytes = max_bytes;
            memcpy(buf_start, row_ptr + area.x / 2, line_bytes);

            if (area.width % 2 == 1 && area.x / 2 + area.width / 2 + 1 < EPD_WIDTH)
                *(buf_start + line_bytes - 1) |= 0xF0;
            if (area.x % 2 == 1 && area.x < EPD_WIDTH) {
                shifted = true;
                uint32_t shift_len = line_bytes + 1;
                uint32_t max_shift = EPD_WIDTH / 2 - (uint32_t)(buf_start - line);
                nibble_shift_buffer_right(buf_start, shift_len < max_shift ? shift_len : max_shift);
            }
            lp = (uint32_t *)line;
        }
        xQueueSendToBack(output_queue, lp, portMAX_DELAY);
        if (shifted)
            memset(line, 255, EPD_WIDTH / 2);
    }
    vTaskDelay(5);
    xSemaphoreGive(params->done_smphr);
    vTaskDelay(portMAX_DELAY);
}

static void IRAM_ATTR feed_display(OutputParams *params) {
    Rect_t area = params->area;
    epd_start_frame();
    for (int32_t i = 0; i < EPD_HEIGHT; i++) {
        if (i < area.y || i >= area.y + area.height) {
            skip_row(contrast_cycles_4[params->frame]);
            continue;
        }
        uint8_t output[EPD_WIDTH / 2];
        xQueueReceive(output_queue, output, portMAX_DELAY);
        calc_epd_input_4bpp((uint32_t *)output, epd_get_current_buffer(), conversion_lut);
        write_row(contrast_cycles_4[params->frame]);
    }
    if (!skipping)
        write_row(contrast_cycles_4[params->frame]);
    epd_end_frame();
    xSemaphoreGive(params->done_smphr);
    vTaskDelay(portMAX_DELAY);
}

static void draw_image(Rect_t area, uint8_t *data) {
    SemaphoreHandle_t fetch_sem = xSemaphoreCreateBinary();
    SemaphoreHandle_t feed_sem = xSemaphoreCreateBinary();
    vTaskDelay(10);
    for (uint8_t k = 0; k < 15; k++) {
        OutputParams p1 = {.data_ptr = data, .done_smphr = fetch_sem, .area = area, .frame = k};
        OutputParams p2 = {.data_ptr = data, .done_smphr = feed_sem,  .area = area, .frame = k};
        TaskHandle_t t1, t2;
        xTaskCreatePinnedToCore((void (*)(void *))provide_out, "epd_provide", 8192, &p1, 10, &t1, 0);
        xTaskCreatePinnedToCore((void (*)(void *))feed_display, "epd_feed",   8192, &p2, 10, &t2, 1);
        xSemaphoreTake(fetch_sem, portMAX_DELAY);
        xSemaphoreTake(feed_sem, portMAX_DELAY);
        vTaskDelete(t1);
        vTaskDelete(t2);
        vTaskDelay(5);
    }
    vSemaphoreDelete(fetch_sem);
    vSemaphoreDelete(feed_sem);
}

// Screen clearing (epd_push_pixels equivalent, simplified)
static void push_pixels(Rect_t area, int16_t time, int32_t color) {
    uint8_t row[EPD_LINE_BYTES] = {0};
    for (uint32_t i = 0; i < (uint32_t)area.width; i++) {
        uint32_t position = i + area.x % 4;
        uint8_t mask = (color ? CLEAR_BYTE : DARK_BYTE) & (0b00000011 << (2 * (position % 4)));
        row[area.x / 4 + position / 4] |= mask;
    }
    reorder_line_buffer((uint32_t *)row);
    epd_start_frame();
    for (int32_t i = 0; i < EPD_HEIGHT; i++) {
        if (i < area.y) {
            skip_row(time);
        } else if (i == area.y) {
            epd_switch_buffer();
            memcpy(epd_get_current_buffer(), row, EPD_LINE_BYTES);
            epd_switch_buffer();
            memcpy(epd_get_current_buffer(), row, EPD_LINE_BYTES);
            write_row(time * 10);
        } else if (i >= area.y + area.height) {
            skip_row(time);
        } else {
            write_row(time * 10);
        }
    }
    write_row(time * 10);
    epd_end_frame();
}

static void clear_screen() {
    Rect_t area = {.x = 0, .y = 0, .width = EPD_WIDTH, .height = EPD_HEIGHT};
    for (int32_t c = 0; c < 4; c++) {
        for (int32_t i = 0; i < 4; i++) push_pixels(area, 50, 0);
        for (int32_t i = 0; i < 4; i++) push_pixels(area, 50, 1);
    }
}

// ---- ESPHome DisplayBuffer overrides ----

void ED047TC1Display::setup() {
    ESP_LOGCONFIG(TAG, "Setting up ED047TC1 display...");

    epd_base_init(EPD_WIDTH);

    conversion_lut = (uint8_t *)heap_caps_malloc(1 << 16, MALLOC_CAP_8BIT);
    if (!conversion_lut) {
        ESP_LOGE(TAG, "Failed to allocate conversion LUT");
        this->mark_failed();
        return;
    }

    output_queue = xQueueCreate(64, EPD_WIDTH / 2);
    if (!output_queue) {
        ESP_LOGE(TAG, "Failed to create output queue");
        this->mark_failed();
        return;
    }

    // Allocate 4bpp framebuffer: 2 pixels per byte
    const uint32_t buf_size = EPD_WIDTH / 2 * EPD_HEIGHT;
    this->init_internal_(buf_size);
    if (!this->buffer_) {
        ESP_LOGE(TAG, "Failed to allocate framebuffer");
        this->mark_failed();
        return;
    }
    memset(this->buffer_, 0xFF, buf_size);

    this->prev_buffer_ = (uint8_t *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!this->prev_buffer_) {
        ESP_LOGE(TAG, "Failed to allocate prev_buffer in PSRAM");
        this->mark_failed();
        return;
    }
    // Initialize to white, matching the hardware state after clear_screen() below.
    memset(this->prev_buffer_, 0xFF, buf_size);

    ESP_LOGCONFIG(TAG, "Initial screen clear...");
    epd_poweron();
    clear_screen();
    epd_poweroff();
    ESP_LOGCONFIG(TAG, "ED047TC1 setup complete");
}

// Convert an ESPHome Color to a 4bpp nibble for this display.
// Vendor LUT convention: nibble 0 = black (maximum darkening), nibble 15 = white (no darkening).
// ESPHome colors are inverted relative to this: COLOR_ON (white/255) → nibble 0 (black ink on paper),
// COLOR_OFF (black/0) → nibble 15 (white paper). So COLOR_OFF is the correct fill for a blank screen.
static uint8_t color_to_nibble(Color color) {
    uint8_t gray = (color.r + color.g + color.b) / 3;
    return 15 - (gray >> 4);
}

void ED047TC1Display::fill(Color color) {
    uint8_t nibble = color_to_nibble(color);
    uint8_t fill_byte = (nibble << 4) | nibble;
    if (this->buffer_)
        memset(this->buffer_, fill_byte, EPD_WIDTH / 2 * EPD_HEIGHT);
}

// Returns the bounding box of pixels that differ between buf_a and buf_b, or
// a zero-area rect {0,0,0,0} if the buffers are identical.
static Rect_t dirty_rect(const uint8_t *buf_a, const uint8_t *buf_b) {
    const int32_t stride = EPD_WIDTH / 2;
    int32_t min_x = EPD_WIDTH, max_x = -1;
    int32_t min_y = EPD_HEIGHT, max_y = -1;
    for (int32_t y = 0; y < EPD_HEIGHT; y++) {
        const uint8_t *row_a = buf_a + y * stride;
        const uint8_t *row_b = buf_b + y * stride;
        for (int32_t b = 0; b < stride; b++) {
            if (row_a[b] != row_b[b]) {
                int32_t px = b * 2;
                if (y     < min_y) min_y = y;
                if (y     > max_y) max_y = y;
                if (px    < min_x) min_x = px;
                if (px + 1 > max_x) max_x = px + 1;
            }
        }
    }
    if (max_x < 0)
        return {.x = 0, .y = 0, .width = 0, .height = 0};
    return {
        .x      = min_x,
        .y      = min_y,
        .width  = max_x - min_x + 1,
        .height = max_y - min_y + 1,
    };
}

void ED047TC1Display::update() {
    this->do_update_();

    const uint32_t buf_size = (EPD_WIDTH / 2) * EPD_HEIGHT;
    Rect_t area = dirty_rect(this->buffer_, this->prev_buffer_);

    if (area.width == 0) {
        ESP_LOGD(TAG, "No pixel changes, skipping hardware update");
        return;
    }

    ESP_LOGD(TAG, "Partial update: (%d,%d) %dx%d", area.x, area.y, area.width, area.height);

    epd_poweron();
    // Drive changed pixels white before rendering. The LUT-based grayscale algorithm
    // only sends darken pulses (never lighten), so previously-dark pixels must be
    // whitened first. Multiple passes are needed because e-paper particles resist
    // moving back to white.
    for (int i = 0; i < 4; i++) push_pixels(area, 50, 1);
    draw_image(area, this->buffer_);
    epd_poweroff();

    memcpy(this->prev_buffer_, this->buffer_, buf_size);
}

void ED047TC1Display::draw_absolute_pixel_internal(int x, int y, Color color) {
    if (x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT || !this->buffer_)
        return;

    uint8_t nibble = color_to_nibble(color);
    uint8_t *ptr = &this->buffer_[y * EPD_WIDTH / 2 + x / 2];
    if (x % 2) {
        *ptr = (*ptr & 0x0F) | (nibble << 4);
    } else {
        *ptr = (*ptr & 0xF0) | nibble;
    }
}

void ED047TC1Display::dump_config() {
    LOG_DISPLAY("", "ED047TC1 E-Paper Display", this);
    ESP_LOGCONFIG(TAG, "  Resolution: %dx%d", EPD_WIDTH, EPD_HEIGHT);
    LOG_UPDATE_INTERVAL(this);
}

}  // namespace ed047tc1
}  // namespace esphome
