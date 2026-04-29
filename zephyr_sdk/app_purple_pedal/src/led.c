#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/drivers/led.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "common.h"

LOG_MODULE_REGISTER(led, CONFIG_APP_LOG_LEVEL);

//#define LED_PWM_NODE_ID	 DT_COMPAT_GET_ANY_STATUS_OKAY(pwm_leds)

ZBUS_CHAN_DECLARE(gamepad_feature_report_raw_val_chan);

static const struct device *led_pwm_pedal = DEVICE_DT_GET(PWM_LED_PEDAL_NODE_ID);

static void gamepad_set_led_brightness(const struct device *dev, uint32_t led, int32_t pedal_value, uint32_t raw_val)
{
    if (raw_val >= LOAD_CELL_DISCONNECT_THRESHOLD) {
        led_set_brightness(dev, led, 0);
        return;
    }

    int32_t safe_pedal = CLAMP(pedal_value, 0, 65535);
    uint8_t brightness = (uint8_t)(((safe_pedal * 97 + 32768) >> 16) + 3);

    int err = led_set_brightness(dev, led, brightness);
    if (err) {
        LOG_ERR("led_set_brightness() returns %d", err);
    }
}

static void gamepad_report_led_cb(const struct zbus_channel *chan)
{
    const struct gamepad_report_out *rpt = zbus_chan_const_msg(chan);
    struct gamepad_feature_rpt_raw_val raw_val = {0};
    
    zbus_chan_read(&gamepad_feature_report_raw_val_chan, &raw_val, K_NO_WAIT);

    gamepad_set_led_brightness(led_pwm_pedal, 0, rpt->accelerator, raw_val.accelerator_raw);
    gamepad_set_led_brightness(led_pwm_pedal, 1, rpt->brake, raw_val.brake_raw);
    gamepad_set_led_brightness(led_pwm_pedal, 2, rpt->clutch, raw_val.clutch_raw);
}
ZBUS_LISTENER_DEFINE(gp_rpt_led_handler, gamepad_report_led_cb);

static const struct device *led_pwm_status = DEVICE_DT_GET(PWM_LED_STATUS_NODE_ID);

#define NUM_STATUS_LED (3)
struct status_led_pattern{
	//uint32_t led[NUM_STATUS_LED];
	uint32_t delay_on[NUM_STATUS_LED]; //Time period (in milliseconds) an LED should be ON
	uint32_t delay_off[NUM_STATUS_LED];
};

const struct status_led_pattern patterns[APP_STATE_NUM] = {
	[APP_STATE_IDLE]	= {.delay_on = {5, 0, 0}, .delay_off = {15, 20, 20}},
	[APP_STATE_CONNECTED] 		= {.delay_on = {5, 5, 0}, .delay_off = {15, 15, 20}},
	[APP_STATE_HID_WORKING] 	= {.delay_on = {0, 5, 0}, .delay_off = {20, 15, 20}},
	[APP_STATE_DFU] 			= {.delay_on = {0, 0, 250}, .delay_off = {500, 500, 250}},
};

static void gamepad_status_led_cb(const struct zbus_channel *chan)
{
	int err;
	const enum app_state *state = zbus_chan_const_msg(chan);
	LOG_DBG("set status LED to status %d", *state);

	for(uint32_t i=0; i<NUM_STATUS_LED; i++){
		err = led_blink(led_pwm_status, i, patterns[*state].delay_on[i], patterns[*state].delay_off[i]);
		if(err){
			LOG_ERR("led_blink() returns %d", err);
		}
	}
}
ZBUS_LISTENER_DEFINE(gp_status_led_handler, gamepad_status_led_cb);

// void gamepad_set_status_led(const enum app_state state)
// {
// 	int err;
// 	LOG_DBG("set status LED to status %d", state);

// 	for(uint32_t i=0; i<NUM_STATUS_LED; i++){
// 		err = led_blink(led_pwm_status, i, patterns[state].delay_on[i], patterns[state].delay_off[i]);
// 		if(err){
// 			LOG_ERR("led_blink() returns %d", err);
// 		}
// 	}
// }