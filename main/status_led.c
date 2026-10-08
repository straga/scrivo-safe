#include "status_led.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"

static const char *TAG = "led";

#if defined(SCRIVO_LED_NEOPIXEL) || defined(SCRIVO_LED_GPIO)
#include "driver/gpio.h"
// The pin comes from board.yml through CMakeLists.txt: one this chip cannot drive stops the build here, not the LED
// staying dark on the board.
_Static_assert(GPIO_IS_VALID_OUTPUT_GPIO(SCRIVO_LED_PIN), "status_led pin in board.yml is not an output pin of this chip");
#endif

#if defined(SCRIVO_LED_NEOPIXEL)

#include "driver/rmt_tx.h"

// One WS2812 through RMT with a callback encoder, after
// examples/peripherals/rmt/led_strip_simple_encoder: no led_strip component needed.
#define RESOLUTION_HZ 10000000   // 0.1 us per tick

// Red, green, blue of each state. 8 of 255 at most: full brightness hurts the eyes in a dark boiler room, and 8 was
// checked by eye on a live board. How bright is this image's choice, not the board's (stage 3.19).
static const uint8_t COLOUR[][3] = {
    [STATUS_LED_RECOVERY] = {8, 3, 0},
    [STATUS_LED_LEAVING] = {0, 0, 0},
};

static const rmt_symbol_word_t bit_zero = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9};
static const rmt_symbol_word_t bit_one = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3};
static const rmt_symbol_word_t reset_code = {.level0 = 0, .duration0 = 250, .level1 = 0, .duration1 = 250};

static rmt_channel_handle_t s_channel;
static rmt_encoder_handle_t s_encoder;

static size_t encode(const void *data, size_t data_size, size_t symbols_written, size_t symbols_free,
                     rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    if (symbols_free < 8) {
        return 0;
    }
    size_t pos = symbols_written / 8;
    if (pos < data_size) {
        const uint8_t byte = ((const uint8_t *)data)[pos];
        for (int i = 0; i < 8; i++) {
            symbols[i] = byte & (0x80 >> i) ? bit_one : bit_zero;
        }
        return 8;
    }
    symbols[0] = reset_code;
    *done = true;
    return 1;
}

void status_led_show(status_led_state_t state)
{
    if (s_channel == NULL) {
        rmt_tx_channel_config_t channel = {
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .gpio_num = SCRIVO_LED_PIN,
            .mem_block_symbols = 64,
            .resolution_hz = RESOLUTION_HZ,
            .trans_queue_depth = 2,
        };
        rmt_simple_encoder_config_t encoder = {.callback = encode};
        if (rmt_new_tx_channel(&channel, &s_channel) != ESP_OK || rmt_new_simple_encoder(&encoder, &s_encoder) != ESP_OK ||
            rmt_enable(s_channel) != ESP_OK) {
            ESP_LOGW(TAG, "LED on GPIO %d not set up", SCRIVO_LED_PIN);
            s_channel = NULL;
            return;
        }
    }
    const uint8_t grb[3] = {COLOUR[state][1], COLOUR[state][0], COLOUR[state][2]};   // WS2812 takes green first
    ESP_LOGI(TAG, "GPIO %d colour %u, %u, %u", SCRIVO_LED_PIN, grb[1], grb[0], grb[2]);
    rmt_transmit_config_t once = {.loop_count = 0};
    if (rmt_transmit(s_channel, s_encoder, grb, sizeof(grb), &once) != ESP_OK ||
        rmt_tx_wait_all_done(s_channel, 100) != ESP_OK) {
        ESP_LOGW(TAG, "LED colour not sent");
    }
}

#elif defined(SCRIVO_LED_GPIO)

#include "esp_timer.h"

// A plain LED flashes briefly once a second in the recovery image: it is the only LED on the board, a steady light
// reads as "powered", and half on, half off reads as a fault. A short flash in a long dark says "alive, not in the
// usual mode", and shows the lit level by eye in a second: a wrong level gives a short dark gap in a long light
// instead (stage 3.19).
#define BLINK_TICK_US     100000   // one tick, and how long the flash lasts
#define BLINK_PERIOD_TICK 10       // a flash every 10 ticks: lit 100 ms of each 1000

static esp_timer_handle_t s_blink;
static volatile bool s_blinking;
static unsigned s_tick;
static bool s_ready;

static void set_lit(bool lit)
{
    gpio_set_level(SCRIVO_LED_PIN, lit ? SCRIVO_LED_ACTIVE : !SCRIVO_LED_ACTIVE);
}

static void blink(void *arg)
{
    if (s_blinking) {
        s_tick = (s_tick + 1) % BLINK_PERIOD_TICK;
        set_lit(s_tick == 0);
    }
}

static bool set_up(void)
{
    const gpio_config_t pin = {.pin_bit_mask = 1ULL << SCRIVO_LED_PIN, .mode = GPIO_MODE_OUTPUT};
    const esp_timer_create_args_t timer = {.callback = blink, .name = "led"};
    if (gpio_config(&pin) != ESP_OK || esp_timer_create(&timer, &s_blink) != ESP_OK) {
        ESP_LOGW(TAG, "LED on GPIO %d not set up", SCRIVO_LED_PIN);
        return false;
    }
    ESP_LOGI(TAG, "GPIO %d lit at level %d: flashes %d ms of each %d ms", SCRIVO_LED_PIN, SCRIVO_LED_ACTIVE,
             BLINK_TICK_US / 1000, BLINK_TICK_US * BLINK_PERIOD_TICK / 1000);
    return true;
}

void status_led_show(status_led_state_t state)
{
    if (!s_ready && !(s_ready = set_up())) {
        return;
    }
    if (state == STATUS_LED_RECOVERY) {
        s_blinking = true;
        s_tick = 0;
        set_lit(true);
        if (!esp_timer_is_active(s_blink)) {
            esp_timer_start_periodic(s_blink, BLINK_TICK_US);
        }
    } else {
        s_blinking = false;
        esp_timer_stop(s_blink);
        set_lit(false);
    }
}

#else

// The board has no status LED (no status_led in its board.yml): nothing to show.
void status_led_show(status_led_state_t state)
{
    static bool told;
    if (!told) {
        told = true;
        ESP_LOGI(TAG, "no status LED on this board");
    }
    (void)state;
}

#endif
