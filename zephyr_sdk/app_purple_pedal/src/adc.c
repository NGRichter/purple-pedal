
#include <zephyr/kernel.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/logging/log.h>

#include "common.h"

LOG_MODULE_REGISTER(app_adc, CONFIG_APP_LOG_LEVEL);

// #define ADC_NODE_ID DT_ALIAS(pedal_adc)
// #define ADC_CHANNEL_COUNT DT_CHILD_NUM(ADC_NODE_ID)
// #define ADC_SAMPLE_PERIOD K_MSEC(10)

// #define CONFIG_SEQUENCE_SAMPLES (1)
// #define ADC_NUM_BITS (12)
#define ADC_VAL_MIN (0)
#define ADC_VAL_MAX (BIT_MASK(ADC_NUM_BITS))
#define ADC_VAL_MID (BIT(ADC_NUM_BITS-1))

#define ADC_GAIN (128)
#define LOAD_CELL_MV_V (1)
#define LOAD_CELL_MAX ((uint64_t)ADC_VAL_MID * ADC_GAIN * LOAD_CELL_MV_V / 1000ULL)

#define APP_ADC_STACK_SIZE 4096
#define APP_ADC_PRIORITY 5

ZBUS_CHAN_DECLARE(gamepad_report_out_chan);
ZBUS_CHAN_DECLARE(gamepad_feature_report_raw_val_chan);

struct app_adc_ctx{
	struct k_work_q *workq;
	struct k_work work;
	const struct device *adc;
	const struct adc_channel_cfg ch_cfgs[ADC_CHANNEL_COUNT];
	const struct adc_sequence_options adc_seq_opt;
	struct adc_sequence seq;
	const struct gamepad_calibration *calibration;
	int32_t channel_reading[CONFIG_SEQUENCE_SAMPLES][ADC_CHANNEL_COUNT];
} ;

// enum adc_action adc_seq_cb(const struct device *dev,const struct adc_sequence *sequence,uint16_t sampling_index)
// {
// 	uint16_t (*reading)[CHANNEL_COUNT] = (uint16_t (*)[CHANNEL_COUNT])sequence->buffer;
// 	struct gamepad_report_out rpt = {
// 		.accelerator = (int32_t)reading[0][0] * (GAMEPAD_REPORT_VALUE_MAX - GAMEPAD_REPORT_VALUE_MIN) / (ADC_VAL_MAX - ADC_VAL_MIN) + GAMEPAD_REPORT_VALUE_MIN,
// 		.brake = (int32_t)reading[0][1] * (GAMEPAD_REPORT_VALUE_MAX - GAMEPAD_REPORT_VALUE_MIN) / (ADC_VAL_MAX - ADC_VAL_MIN) + GAMEPAD_REPORT_VALUE_MIN,
// 		.clutch = (int32_t)reading[0][2] * (GAMEPAD_REPORT_VALUE_MAX - GAMEPAD_REPORT_VALUE_MIN) / (ADC_VAL_MAX - ADC_VAL_MIN) + GAMEPAD_REPORT_VALUE_MIN,
// 	};
// 	int err = zbus_chan_pub(&gamepad_report_out_chan, &rpt, K_MSEC(2));

// 	struct app_adc_ctx *ctx = (struct app_adc_ctx *)sequence->options->user_data;
// 	err = k_sem_take(&ctx->sem_stop, K_NO_WAIT);
// 	if(err == 0)
// 		return ADC_ACTION_FINISH;
// 	else
// 		return ADC_ACTION_REPEAT;
// }

//uint16_t channel_reading[CONFIG_SEQUENCE_SAMPLES][CHANNEL_COUNT];

//forward declaration
static void app_adc_work_handler(struct k_work *work);

K_THREAD_STACK_DEFINE(app_adc_workq_stack_area, APP_ADC_STACK_SIZE);
static struct k_work_q app_adc_workq;
static struct app_adc_ctx ctx = {
	.workq = &app_adc_workq,
	.work = Z_WORK_INITIALIZER(app_adc_work_handler),
	.adc = DEVICE_DT_GET(ADC_NODE_ID),
	.ch_cfgs={DT_FOREACH_CHILD_SEP(ADC_NODE_ID, ADC_CHANNEL_CFG_DT, (,))},
	.adc_seq_opt = {
		.extra_samplings = CONFIG_SEQUENCE_SAMPLES - 1,
		.interval_us = 0,
		//.callback = adc_seq_cb,
		.user_data = &ctx,
	},
	.seq = {
		.channels = BIT_MASK(ADC_CHANNEL_COUNT),
		.buffer = ctx.channel_reading,
		.buffer_size = 	SIZEOF_FIELD(struct app_adc_ctx, channel_reading),
		//.resolution = 12,
		.resolution = 24,
		//.resolution = DT_PROP(ADC_NODE_ID, resolution),
		.options = &ctx.adc_seq_opt,
	},
	//.sem_stop = Z_SEM_INITIALIZER(ctx.sem_stop, 0, 1),
};

// static void app_adc_work_handler(struct k_work *work)
// {
// 	struct app_adc_ctx *ctx = CONTAINER_OF(work, struct app_adc_ctx, work);
// 	int err;
// 	if (!device_is_ready(ctx->adc)) {
// 		LOG_ERR("ADC controller device %s not ready\n", ctx->adc->name);
// 	}
// 	for (size_t i = 0U; i < CHANNEL_COUNT; i++) {
// 		err = adc_channel_setup(ctx->adc, &ctx->ch_cfgs[i]);
// 		if (err < 0) {
// 			LOG_ERR("Could not setup channel #%d (%d)\n", i, err);
// 		}
// 	}
// 	err = adc_read(ctx->adc, &ctx->seq);
// 	if (err < 0){
// 		LOG_ERR("adc_read() error (%d)\n", err);
// 	}
// 	LOG_DBG("app_adc_work_handler() ended");
// }

static inline uint16_t raw_to_uint16(int32_t raw, int32_t offset, int32_t scale)
{
	//clamp the <offset values. also when loadcell is disconnect, we should output 0
	if(raw < offset || raw >= LOAD_CELL_DISCONNECT_THRESHOLD) return 0;

	int64_t val_64 = ((int64_t)(raw - offset)) * UINT16_MAX / scale;
	return (val_64 > UINT16_MAX)? UINT16_MAX : (uint16_t)val_64;
}

static uint16_t curve_calculate(uint16_t in, const uint16_t *curve_points, uint8_t num_points)
{
    if (num_points < 2) return in;
    
    // Clamp max value instantly to prevent overflow
    if (in >= 65535) {
        return curve_points[num_points - 1];
    }

    uint8_t shift;
    uint16_t mask;

    // Fast-path for optimized Power-of-2 + 1 resolutions
    switch (num_points) {
        case 65: // 64 intervals -> Width 1024
            shift = 10;
            mask = 0x03FF;
            break;
        case 33: // 32 intervals -> Width 2048
            shift = 11;
            mask = 0x07FF;
            break;
        case 17: // 16 intervals -> Width 4096
            shift = 12;
            mask = 0x0FFF;
            break;
        case 9:  // 8 intervals -> Width 8192
            shift = 13;
            mask = 0x1FFF;
            break;
        case 5:  // 4 intervals -> Width 16384
            shift = 14;
            mask = 0x3FFF;
            break;
            
        // Slow-path fallback for custom/irregular array sizes
        default: {
            uint32_t intervals = num_points - 1;
            uint32_t interval_width = 65536 / intervals; 
            
            uint32_t idx = in / interval_width;
            if (idx >= intervals) {
                idx = intervals - 1;
            }
            
            uint32_t x = in % interval_width;
            int32_t y0 = (int32_t)curve_points[idx];
            int32_t y1 = (int32_t)curve_points[idx + 1];
            
            int32_t interpolated = y0 + (((y1 - y0) * (int32_t)x) / (int32_t)interval_width);
            
            if (interpolated < 0) return 0;
            if (interpolated > 65535) return 65535;
            return (uint16_t)interpolated;
        }
    }

    // --- Execution for Fast-Path (Bitwise Math) ---
    
    // Bit-shift determines array index
    uint32_t idx = in >> shift; 
    
    // Safety clamp (Hardware guarantees this won't hit if 'in' < 65535, but safe for embedded)
    if (idx >= (num_points - 1)) {
        idx = num_points - 2;
    }

    // Bit-mask determines interpolation progress
    uint32_t x = in & mask;

    int32_t y0 = (int32_t)curve_points[idx];
    int32_t y1 = (int32_t)curve_points[idx + 1];

    int32_t delta = y1 - y0;

    // Bit-shift handles the final division
    int32_t interpolated = y0 + ((delta * (int32_t)x) >> shift);

    // Hard clamp to prevent uint16_t wrap-around loops
    if (interpolated < 0) return 0;
    if (interpolated > 65535) return 65535;

    return (uint16_t)interpolated;
}

static void app_adc_work_handler(struct k_work *work)
{
	struct app_adc_ctx *ctx = CONTAINER_OF(work, struct app_adc_ctx, work);
	//int64_t begin = k_uptime_get();
	int err = adc_read(ctx->adc, &ctx->seq);
	//LOG_DBG("adc time: %d ms", (int32_t)(k_uptime_get()-begin));
	if (err < 0){
		LOG_ERR("adc_read() error (%d)\n", err);
	}
	// struct gamepad_report_out rpt = {
	// 	.accelerator = (int64_t)ctx->channel_reading[0][0] * (GAMEPAD_REPORT_VALUE_MAX - GAMEPAD_REPORT_VALUE_MIN) / (ADC_VAL_MAX - ADC_VAL_MIN) + GAMEPAD_REPORT_VALUE_MIN,
	// 	.brake = (int64_t)ctx->channel_reading[0][1] * (GAMEPAD_REPORT_VALUE_MAX - GAMEPAD_REPORT_VALUE_MIN) / (ADC_VAL_MAX - ADC_VAL_MIN) + GAMEPAD_REPORT_VALUE_MIN,
	// 	.clutch = (int64_t)ctx->channel_reading[0][2] * (GAMEPAD_REPORT_VALUE_MAX - GAMEPAD_REPORT_VALUE_MIN) / (ADC_VAL_MAX - ADC_VAL_MIN) + GAMEPAD_REPORT_VALUE_MIN,
	// };

	// struct gamepad_report_out rpt = {
	// 	.report_id = GAMEPAD_INPUT_REPORT_ID,
	// 	.accelerator = 
	// 	((int64_t)(ctx->channel_reading[0][0] - ADC_VAL_MID))
	// 	*32768
	// 	/LOAD_CELL_MAX,

	// 	.brake = 
	// 	((int64_t)(ctx->channel_reading[0][1] - ADC_VAL_MID))
	// 	*32768
	// 	/LOAD_CELL_MAX,

	// 	.clutch = 
	// 	((int64_t)(ctx->channel_reading[0][2] - ADC_VAL_MID))
	// 	*32768
	// 	/LOAD_CELL_MAX,
	// };

	struct gamepad_feature_rpt_raw_val rpt_raw_val = {
		.report_id = GAMEPAD_FEATURE_REPORT_RAW_VAL_ID,
		.accelerator_raw = ctx->channel_reading[0][SETTING_INDEX_ACCELERATOR],
		.brake_raw = ctx->channel_reading[0][SETTING_INDEX_BRAKE],
		.clutch_raw = ctx->channel_reading[0][SETTING_INDEX_CLUTCH],
	};

	// LOG_INF("rpt_raw_val.acc_raw = 0x%x, .brake_raw = 0x%x, .clutch_raw = 0x%x", 
	// 	rpt_raw_val.accelerator_raw, rpt_raw_val.brake_raw, rpt_raw_val.clutch_raw);

	// struct gamepad_report_out rpt = {
	// 	.report_id = GAMEPAD_INPUT_REPORT_ID,
	// 	.accelerator = 
	// 		raw_to_uint16(rpt_raw_val.accelerator_raw, 
	// 				ctx->calibration->offset[SETTING_INDEX_ACCELERATOR], 
	// 				ctx->calibration->scale[SETTING_INDEX_ACCELERATOR]),
	// 	.brake = raw_to_uint16(rpt_raw_val.brake_raw, 
	// 				ctx->calibration->offset[SETTING_INDEX_BRAKE], 
	// 				ctx->calibration->scale[SETTING_INDEX_BRAKE]),
	// 	.clutch = raw_to_uint16(rpt_raw_val.clutch_raw, 
	// 				ctx->calibration->offset[SETTING_INDEX_CLUTCH], 
	// 				ctx->calibration->scale[SETTING_INDEX_CLUTCH]),
	// };

	// Calculate using the pure ADC buffer directly, bypassing the 24-bit packed struct
	uint16_t acc_tmp = raw_to_uint16(
					ctx->channel_reading[0][SETTING_INDEX_ACCELERATOR], 
					ctx->calibration->offset[SETTING_INDEX_ACCELERATOR], 
					ctx->calibration->scale[SETTING_INDEX_ACCELERATOR]);
					
	uint16_t brake_tmp = raw_to_uint16(
					ctx->channel_reading[0][SETTING_INDEX_BRAKE], 
					ctx->calibration->offset[SETTING_INDEX_BRAKE], 
					ctx->calibration->scale[SETTING_INDEX_BRAKE]);
					
	uint16_t clutch_tmp = raw_to_uint16(
					ctx->channel_reading[0][SETTING_INDEX_CLUTCH], 
					ctx->calibration->offset[SETTING_INDEX_CLUTCH], 
					ctx->calibration->scale[SETTING_INDEX_CLUTCH]);

	//TODO: apply curve here
	const struct gamepad_curve* curve = get_active_curve_slot();
	uint16_t acc_final = curve_calculate(acc_tmp, curve->accelerator, GAMEPAD_FEATURE_REPORT_CURVE_NUM_POINTS);
	uint16_t brake_final = curve_calculate(brake_tmp, curve->brake, GAMEPAD_FEATURE_REPORT_CURVE_NUM_POINTS);
	uint16_t clutch_final = curve_calculate(clutch_tmp, curve->clutch, GAMEPAD_FEATURE_REPORT_CURVE_NUM_POINTS);

	struct gamepad_report_out rpt = {
		.report_id = GAMEPAD_INPUT_REPORT_ID,
		.accelerator = acc_final,
		.brake = brake_final,
		.clutch = clutch_final,
	};

	//LOG_INF("gamepad report: accelerator = %d, brake = %d, clutch = %d",rpt.accelerator, rpt.brake, rpt.clutch);

	// struct gamepad_report_out rpt = {
	// 	.accelerator = 100,
	// 	.brake = 200,
	// 	.clutch = 300,
	// };
	//int64_t acc = (int64_t)ctx->channel_reading[0][0] * (GAMEPAD_REPORT_VALUE_MAX - GAMEPAD_REPORT_VALUE_MIN) / (ADC_VAL_MAX - ADC_VAL_MIN);
	//LOG_ERR("acc(%lld)\n", (ADC_VAL_MAX - ADC_VAL_MIN));
	err = zbus_chan_pub(&gamepad_report_out_chan, &rpt, ADC_SAMPLE_PERIOD);
	if (err < 0){
		LOG_ERR("zbus_chan_pub(&gamepad_report_out_chan) error (%d)\n", err);
	}
	err = zbus_chan_pub(&gamepad_feature_report_raw_val_chan, &rpt_raw_val, ADC_SAMPLE_PERIOD);
	if (err < 0){
		LOG_ERR("zbus_chan_pub(&gamepad_feature_report_raw_val_chan) error (%d)\n", err);
	}				
	//LOG_DBG("app_adc_work_handler() ended");
}

static void app_adc_timer_handler(struct k_timer *timer)
{
	struct app_adc_ctx *ctx = (struct app_adc_ctx *)k_timer_user_data_get(timer);
	//check if the work is already busy, avoid resubmitting
	if (k_work_busy_get(&ctx->work)){
		LOG_INF("adc already working.");
	}
	else{
		int err = k_work_submit_to_queue(ctx->workq, &ctx->work);
		if (err < 0){
			LOG_ERR("k_work_submit_to_queue() returns %d", err);
		}
	}
}
static K_TIMER_DEFINE(app_adc_timer, app_adc_timer_handler, NULL);

// static void gamepad_adc_ctrl_cb(const struct zbus_channel *chan)
// {
// 	int err;
// 	const enum app_adc_action *action = zbus_chan_const_msg(chan);

// 	//TODO: perform start / stop actions
// 	if(*action == APP_ADC_ACTION_START){
// 		//check if the work is already busy, avoid resubmitting
// 		if(k_work_busy_get(&ctx.work)){
// 			LOG_INF("adc already working.");
// 		}
// 		else{
// 			err = k_work_submit_to_queue(&app_adc_work_q, &ctx.work);
// 			if(err<0){
// 				LOG_ERR("k_work_submit_to_queue() returns %d", err);
// 			}
// 		}
// 		return;
// 	}

// 	if(*action == APP_ADC_ACTION_STOP){
// 		k_sem_give(&ctx.sem_stop);
// 		return;
// 	}
// }

// static void gamepad_adc_ctrl_cb(const struct zbus_channel *chan)
// {
// 	const enum app_adc_action *action = zbus_chan_const_msg(chan);
// 	//perform start / stop actions
// 	if(*action == APP_ADC_ACTION_START){
// 		k_timer_start(&app_adc_timer, K_MSEC(0), ADC_SAMPLE_PERIOD);
// 		return;
// 	}

// 	if(*action == APP_ADC_ACTION_STOP){
// 		k_timer_stop(&app_adc_timer);
// 		return;
// 	}
// }
// ZBUS_LISTENER_DEFINE(gp_adc_ctrl_handler, gamepad_adc_ctrl_cb);

static void gamepad_adc_ctrl_cb(const struct zbus_channel *chan)
{
	const enum app_state *state = zbus_chan_const_msg(chan);
	if(*state == APP_STATE_HID_WORKING){
		LOG_DBG("start ADC timer");
		k_timer_start(&app_adc_timer, K_MSEC(0), ADC_SAMPLE_PERIOD);
		return;
	}
	else{
		LOG_DBG("stop ADC timer");
		k_timer_stop(&app_adc_timer);
		return;
	}
}
ZBUS_LISTENER_DEFINE(gp_adc_ctrl_handler, gamepad_adc_ctrl_cb);

int app_adc_init(void)
{
	int err;
	if (!device_is_ready(ctx.adc)){
		LOG_ERR("ADC controller device %s not ready\n", ctx.adc->name);
	}
	for (size_t i = 0U; i < ADC_CHANNEL_COUNT; i++){
		err = adc_channel_setup(ctx.adc, &ctx.ch_cfgs[i]);
		if (err < 0){
			LOG_ERR("Could not setup channel #%d (%d)\n", i, err);
		}
	}
	k_timer_user_data_set(&app_adc_timer, &ctx);
	k_work_queue_init(&app_adc_workq);
	k_work_queue_start(&app_adc_workq, app_adc_workq_stack_area,K_THREAD_STACK_SIZEOF(app_adc_workq_stack_area), APP_ADC_PRIORITY,NULL);

	//get the pointer to calibration settings
	ctx.calibration = get_calibration();
	return 0;
}
