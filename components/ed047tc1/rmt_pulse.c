
/******************************************************************************/
/***        include files                                                   ***/
/******************************************************************************/

#include "rmt_pulse.h"

#include <driver/rmt_tx.h>
#include <driver/rmt_encoder.h>
#include <freertos/FreeRTOS.h>  // portMAX_DELAY

#include <esp_log.h>

/******************************************************************************/
/***        local variables                                                 ***/
/******************************************************************************/

static rmt_channel_handle_t rmt_chan = NULL;
static rmt_encoder_handle_t copy_encoder = NULL;

/******************************************************************************/
/***        exported functions                                              ***/
/******************************************************************************/

void rmt_pulse_init(gpio_num_t pin)
{
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = pin,
        .mem_block_symbols = 64,
        // 10 MHz -> 0.1 us resolution (same as legacy clk_div=8 on 80MHz APB)
        .resolution_hz = 10 * 1000 * 1000,
        .trans_queue_depth = 4,
        .flags.invert_out = false,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &rmt_chan));

    rmt_copy_encoder_config_t copy_encoder_config = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&copy_encoder_config, &copy_encoder));

    ESP_ERROR_CHECK(rmt_enable(rmt_chan));
}


void IRAM_ATTR pulse_ckv_ticks(uint16_t high_time_ticks,
                               uint16_t low_time_ticks, bool wait)
{
    rmt_symbol_word_t symbol;
    if (high_time_ticks > 0) {
        symbol.level0    = 1;
        symbol.duration0 = high_time_ticks;
        symbol.level1    = 0;
        symbol.duration1 = low_time_ticks > 0 ? low_time_ticks : 1;
    } else {
        symbol.level0    = 1;
        symbol.duration0 = low_time_ticks;
        symbol.level1    = 0;
        symbol.duration1 = 1;
    }

    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    rmt_transmit(rmt_chan, copy_encoder, &symbol, sizeof(symbol), &tx_config);
    if (wait) {
        rmt_tx_wait_all_done(rmt_chan, portMAX_DELAY);
    }
}


void IRAM_ATTR pulse_ckv_us(uint16_t high_time_us, uint16_t low_time_us, bool wait)
{
    pulse_ckv_ticks(10 * high_time_us, 10 * low_time_us, wait);
}

/******************************************************************************/
/***        END OF FILE                                                     ***/
/******************************************************************************/
