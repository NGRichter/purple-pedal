#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "common.h"
LOG_MODULE_REGISTER(nvs_settings, CONFIG_APP_LOG_LEVEL);

#define SETTING_GAMEPAD_ROOT "gamepad"

#define SETTING_SUB_CALIBRATION "calib"

#define SETTING_SUB_CURVE "curve"
#define SETTING_SUB_CURVE_ACTIVE "active"
#define SETTING_SUB_CURVE_SLOT "slot"

#define SETTING_SUB_CALIB_ACTIVE "c_active"
#define SETTING_SUB_CALIB_SLOT "c_slot"

#define SETTING_CALIBRATION SETTING_GAMEPAD_ROOT"/"SETTING_SUB_CALIBRATION
#define SETTING_CURVE_ACTIVE SETTING_GAMEPAD_ROOT"/"SETTING_SUB_CURVE"/"SETTING_SUB_CURVE_ACTIVE
#define SETTING_CURVE_SLOT SETTING_GAMEPAD_ROOT"/"SETTING_SUB_CURVE"/"SETTING_SUB_CURVE_SLOT

#define SETTING_CALIB_ACTIVE SETTING_GAMEPAD_ROOT"/"SETTING_SUB_CALIB_ACTIVE
#define SETTING_CALIB_SLOT SETTING_GAMEPAD_ROOT"/"SETTING_SUB_CALIB_SLOT

#define SETTING_CALIBRATION_LEN (sizeof(struct gamepad_calibration))
#define SETTING_CURVE_ACTIVE_LEN (SIZEOF_FIELD(struct gamepad_feature_rpt_active_curve, active_curve_slot))

// static const char *SETTING_CURVE_SLOT_STRS[GAMEPAD_FEATURE_REPORT_CURVE_SLOT_NUM] = {
// 	//Note: slot0 is the defaut slot that only lives in RAM
// 	SETTING_CURVE_SLOT(1),
// 	SETTING_CURVE_SLOT(2),
// 	SETTING_CURVE_SLOT(3),
// 	SETTING_CURVE_SLOT(4),
// 	SETTING_CURVE_SLOT(5),
// };

static struct gamepad_calibration gp_calibration = {
    .offset = {[0 ... SETTING_INDEX_TOTAL-1] = LOAD_CELL_DEFAULT_OFFSET},
    .scale = {[0 ... SETTING_INDEX_TOTAL-1] = LOAD_CELL_DEFAULT_SCALE},
};

static struct gamepad_curve_context gp_curve_ctx = {
	.active_curve_slot = 0,
	.curve_slot = {
		[0 ... GAMEPAD_TOTAL_CURVE_SLOT_NUM-1] = {
			.accelerator = {[0 ... GAMEPAD_FEATURE_REPORT_CURVE_NUM_POINTS-1] = 0},
			.brake = {[0 ... GAMEPAD_FEATURE_REPORT_CURVE_NUM_POINTS-1] = 0},
			.clutch = {[0 ... GAMEPAD_FEATURE_REPORT_CURVE_NUM_POINTS-1] = 0},
		}
	}
};

static struct gamepad_calib_context gp_calib_ctx;

int gampepad_setting_handle_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	int rc;
	const char *next;
	if (settings_name_steq(name, SETTING_SUB_CALIB_ACTIVE, &next) && !next) {
		if (len != sizeof(uint8_t)) {
			return -EINVAL;
		}
		rc = read_cb(cb_arg, &gp_calib_ctx.active_calib_slot, sizeof(uint8_t));
		if (rc >= 0) {
			return 0;
		}
 	   return rc;
	}

	if (settings_name_steq(name, SETTING_SUB_CALIB_SLOT, &next) && next) {
		if (*next < '1' || *next > ('0' + GAMEPAD_TOTAL_CALIB_SLOT_NUM)) {
			return -EINVAL;
		}
		uint8_t slot = *next - '0';

		if (len != sizeof(struct gamepad_calibration)) {
			return -EINVAL;
		}

		rc = read_cb(cb_arg, &gp_calib_ctx.calib_slot[slot - 1], sizeof(struct gamepad_calibration));
		if (rc >= 0) {
			return 0;
		}
		return rc;
	}

	if (settings_name_steq(name, SETTING_SUB_CURVE"/"SETTING_SUB_CURVE_ACTIVE, &next) && !next) {
		if (len != SETTING_CURVE_ACTIVE_LEN) {
			return -EINVAL;
		}
		rc = read_cb(cb_arg, &gp_curve_ctx.active_curve_slot, SETTING_CURVE_ACTIVE_LEN);
		if (rc >= 0) {
			return 0;
		}
		return rc;
	}

	if(settings_name_steq(name, SETTING_SUB_CURVE"/"SETTING_SUB_CURVE_SLOT, &next) && next){
		if(*next < '1' || *next > '5'){
			return -EINVAL;
		}
		uint8_t slot = *next - '0';

		if(len != sizeof(struct gamepad_curve)){
			return -EINVAL;
		}

		rc = read_cb(cb_arg, &gp_curve_ctx.curve_slot[slot], sizeof(struct gamepad_curve));
		if(rc >= 0){
			return 0;
		}
		return rc;
	}

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(gamepad, SETTING_GAMEPAD_ROOT, NULL, gampepad_setting_handle_set, NULL, NULL);

int app_setting_init(void)
{
	//initialize default values for curves
	for(size_t slot=0; slot<GAMEPAD_TOTAL_CURVE_SLOT_NUM; slot++){
		for(size_t i=0; i<GAMEPAD_FEATURE_REPORT_CURVE_NUM_POINTS; i++){
			uint16_t val = (uint16_t)(UINT16_MAX * i / (GAMEPAD_FEATURE_REPORT_CURVE_NUM_POINTS-1));
			gp_curve_ctx.curve_slot[slot].accelerator[i] = val;
			gp_curve_ctx.curve_slot[slot].brake[i] = val;
			gp_curve_ctx.curve_slot[slot].clutch[i] = val;
		}
	}

	gp_calib_ctx.active_calib_slot = 1;
	for (size_t slot = 0; slot < GAMEPAD_TOTAL_CALIB_SLOT_NUM; slot++) {
		gp_calib_ctx.calib_slot[slot] = (struct gamepad_calibration) {
			.offset = {
				[0 ... SETTING_INDEX_TOTAL-1] = LOAD_CELL_DEFAULT_OFFSET,
				[1] = LOAD_CELL_INDEX_1_OFFSET
			},
			.scale = {
				[0 ... SETTING_INDEX_TOTAL-1] = LOAD_CELL_DEFAULT_SCALE,
				[1] = LOAD_CELL_INDEX_1_SCALE
			}
		};
	}

	int rc = settings_subsys_init();
	if (rc) {
		LOG_ERR("settings subsys initialization: fail (err %d)", rc);
		return -EINVAL;
	}
	rc = settings_load();
	if (rc) {
		LOG_ERR("settings subsys load: fail (err %d)", rc);
		return -EINVAL;
	}
	LOG_DBG("settings subsys initialization: OK.");
	return 0;

}

const struct gamepad_calibration *get_calibration(void)
{
    return &gp_calibration;
}

int set_calibration(const struct gamepad_calibration *calib)
{
    int err = settings_save_one(SETTING_CALIBRATION, calib, SETTING_CALIBRATION_LEN);
    if(err) return err;
    return settings_load_subtree(SETTING_CALIBRATION);
}

int set_active_curve(uint8_t slot)
{
	if(slot > GAMEPAD_FEATURE_REPORT_CURVE_SLOT_NUM){
		return -EINVAL;
	}
	//TODO: shall we set to default slot0 value?
	int err = settings_save_one(SETTING_CURVE_ACTIVE, &slot, sizeof(slot));
	if(err) return err;
	return settings_load_subtree(SETTING_CURVE_ACTIVE);
}

uint8_t get_active_curve(void)
{
	return gp_curve_ctx.active_curve_slot;
}

int set_curve_slot(uint8_t slot_id, const struct gamepad_curve *curve)
{
	uint8_t slot = slot_id - GAMEPAD_FEATURE_REPORT_CURVE_SLOT_ID_BASE;
	if(slot == 0 || slot > GAMEPAD_FEATURE_REPORT_CURVE_SLOT_NUM){
		return -EINVAL;
	}
	char setting_name[sizeof(SETTING_CURVE_SLOT) + 2]; //+2 for / and slot number
	snprintf(setting_name, sizeof(setting_name), SETTING_CURVE_SLOT"/""%d", slot);
	int err = settings_save_one(setting_name, curve, sizeof(struct gamepad_curve));
	if(err) return err;
	return settings_load_subtree(setting_name);
}

const struct gamepad_curve* get_curve_slot(uint8_t slot_id)
{	
	uint8_t slot = slot_id - GAMEPAD_FEATURE_REPORT_CURVE_SLOT_ID_BASE;
	if(slot > GAMEPAD_FEATURE_REPORT_CURVE_SLOT_NUM){
		return NULL;
	}
	return &gp_curve_ctx.curve_slot[slot];
}

const struct gamepad_curve* get_active_curve_slot(void)
{
	return get_curve_slot(gp_curve_ctx.active_curve_slot + GAMEPAD_FEATURE_REPORT_CURVE_SLOT_ID_BASE);
}

int set_active_calib(uint8_t slot)
{
    if (slot == 0 || slot > GAMEPAD_TOTAL_CALIB_SLOT_NUM) {
        return -EINVAL;
    }
    
    int err = settings_save_one(SETTING_CALIB_ACTIVE, &slot, sizeof(slot));
    if (err) return err;
    
    gp_calib_ctx.active_calib_slot = slot;
    return 0;
}

uint8_t get_active_calib(void)
{
    return gp_calib_ctx.active_calib_slot;
}

int set_calib_slot(uint8_t slot_id, const struct gamepad_calibration *calib)
{
    uint8_t slot = slot_id - GAMEPAD_FEATURE_REPORT_CALIB_SLOT_ID_BASE;
    if (slot == 0 || slot > GAMEPAD_TOTAL_CALIB_SLOT_NUM) {
        return -EINVAL;
    }

    char setting_name[sizeof(SETTING_CALIB_SLOT) + 3]; 
    snprintf(setting_name, sizeof(setting_name), SETTING_CALIB_SLOT"/%d", slot);
    
    int err = settings_save_one(setting_name, calib, sizeof(struct gamepad_calibration));
    if (err) return err;

    memcpy(&gp_calib_ctx.calib_slot[slot - 1], calib, sizeof(struct gamepad_calibration));
    return 0;
}

int get_calib_slot(uint8_t slot_id, struct gamepad_calibration *calib)
{
    uint8_t slot = slot_id - GAMEPAD_FEATURE_REPORT_CALIB_SLOT_ID_BASE;
    if (slot == 0 || slot > GAMEPAD_TOTAL_CALIB_SLOT_NUM) {
        return -EINVAL;
    }
    memcpy(calib, &gp_calib_ctx.calib_slot[slot - 1], sizeof(struct gamepad_calibration));
    return 0;
}

struct gamepad_calibration* get_current_active_calib_ptr(void)
{
    uint8_t slot = gp_calib_ctx.active_calib_slot;
    if (slot == 0 || slot > GAMEPAD_TOTAL_CALIB_SLOT_NUM) {
        slot = 1; // Fallback to slot 1 if uninitialized
    }
    return &gp_calib_ctx.calib_slot[slot - 1];
}