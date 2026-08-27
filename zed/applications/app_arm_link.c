#include "app_arm_link.h"
#include "drv_MG996.h"
#include <math.h>
#include <stdbool.h>
#include <string.h>

#define ARM_SERVO_SPEED_STEP_DEFAULT         2.0f
#define ARM_EMG_SERVO0_OPEN_ANGLE_DEFAULT    0.0f
#define ARM_EMG_SERVO0_CLOSE_ANGLE_DEFAULT   90.0f
#define ARM_EMG_SERVO0_ENV_OPEN_DEFAULT      8.0f
#define ARM_EMG_SERVO0_ENV_CLOSE_DEFAULT     60.0f
#define ARM_EMG_SERVO0_ENV_ALPHA_DEFAULT     0.20f
#define ARM_EMG_SERVO0_GRIP_ALPHA_DEFAULT    0.30f
#define ARM_EMG_SERVO0_DEADBAND_DEFAULT      6.0f
#define ARM_EMG_SERVO0_HOLD_DEFAULT          1.0f
#define ARM_EMG_SERVO0_SPEED_STEP_DEFAULT    0.5f
#define ARM_EMG_SERVO0_MIN_ENV_SPAN          1.0f

#define CODE_SERVO5_OFFSET     0.0f
#define CODE_SERVO4_OFFSET     0.0f
#define CODE_SERVO3_OFFSET     0.0f
#define CODE_SERVO2_OFFSET     0.0f
#define CODE_SERVO1_OFFSET     0.0f

#define CODE_SERVO5_REVERSE    0
#define CODE_SERVO4_REVERSE    0
#define CODE_SERVO3_REVERSE    0
#define CODE_SERVO2_REVERSE    0
#define CODE_SERVO1_REVERSE    0

#define SHOULDER_SPLIT_DEG     80.0f
#define SERVO3_SHOULDER_BASE   90.0f

typedef struct st_arm_emg_servo0_state
{
    bool                 initialized;
    arm_emg_servo0_cfg_t cfg;
    float                filtered_envelope;
    float                grip_percent_f;
} arm_emg_servo0_state_t;

static arm_emg_servo0_state_t s_arm_emg_servo0;
static float                  s_arm_joint_speed_step = ARM_SERVO_SPEED_STEP_DEFAULT;

static float clampf(float x, float min_v, float max_v);
static float apply_reverse_and_offset(float angle, bool reverse, float offset);
static void  map_shoulder_split(float shoulder_deg, float * p_out_servo4, float * p_out_servo3);
static void  arm_emg_servo0_load_defaults(arm_emg_servo0_cfg_t * p_cfg);
static void  arm_emg_servo0_ensure_initialized(void);
static float arm_emg_servo0_compute_target_grip_percent(float filtered_envelope);

static float clampf(float x, float min_v, float max_v)
{
    if (x < min_v)
    {
        return min_v;
    }

    if (x > max_v)
    {
        return max_v;
    }

    return x;
}

static float apply_reverse_and_offset(float angle, bool reverse, float offset)
{
    float out = angle;

    if (reverse)
    {
        out = 180.0f - out;
    }

    out += offset;

    return out;
}

static void map_shoulder_split(float shoulder_deg, float * p_out_servo4, float * p_out_servo3)
{
    float servo4_angle;
    float servo3_angle;

    shoulder_deg = clampf(shoulder_deg, 0.0f, 180.0f);

    if (shoulder_deg <= SHOULDER_SPLIT_DEG)
    {
        servo4_angle = 90.0f - shoulder_deg;
        servo3_angle = SERVO3_SHOULDER_BASE;
    }
    else
    {
        servo4_angle = 10.0f;
        servo3_angle = SERVO3_SHOULDER_BASE + (shoulder_deg - SHOULDER_SPLIT_DEG);
    }

    servo4_angle = apply_reverse_and_offset(servo4_angle, CODE_SERVO4_REVERSE, CODE_SERVO4_OFFSET);
    servo3_angle = apply_reverse_and_offset(servo3_angle, CODE_SERVO3_REVERSE, CODE_SERVO3_OFFSET);

    *p_out_servo4 = clampf(servo4_angle, 0.0f, 180.0f);
    *p_out_servo3 = clampf(servo3_angle, 0.0f, 180.0f);
}

static void arm_emg_servo0_load_defaults(arm_emg_servo0_cfg_t * p_cfg)
{
    if (NULL == p_cfg)
    {
        return;
    }

    p_cfg->envelope_open = ARM_EMG_SERVO0_ENV_OPEN_DEFAULT;
    p_cfg->envelope_close = ARM_EMG_SERVO0_ENV_CLOSE_DEFAULT;
    p_cfg->envelope_alpha = ARM_EMG_SERVO0_ENV_ALPHA_DEFAULT;
    p_cfg->grip_alpha = ARM_EMG_SERVO0_GRIP_ALPHA_DEFAULT;
    p_cfg->grip_deadband_percent = ARM_EMG_SERVO0_DEADBAND_DEFAULT;
    p_cfg->grip_hold_percent = ARM_EMG_SERVO0_HOLD_DEFAULT;
    p_cfg->servo_open_angle = ARM_EMG_SERVO0_OPEN_ANGLE_DEFAULT;
    p_cfg->servo_close_angle = ARM_EMG_SERVO0_CLOSE_ANGLE_DEFAULT;
    p_cfg->servo_speed_step = ARM_EMG_SERVO0_SPEED_STEP_DEFAULT;
}

static void arm_emg_servo0_ensure_initialized(void)
{
    if (!s_arm_emg_servo0.initialized)
    {
        arm_emg_servo0_load_defaults(&s_arm_emg_servo0.cfg);
        s_arm_emg_servo0.filtered_envelope = 0.0f;
        s_arm_emg_servo0.grip_percent_f = 0.0f;
        s_arm_emg_servo0.initialized = true;
    }
}

static float arm_emg_servo0_compute_target_grip_percent(float filtered_envelope)
{
    float span;
    float normalized;
    float grip_percent;

    arm_emg_servo0_ensure_initialized();

    span = s_arm_emg_servo0.cfg.envelope_close - s_arm_emg_servo0.cfg.envelope_open;
    if (span < ARM_EMG_SERVO0_MIN_ENV_SPAN)
    {
        span = ARM_EMG_SERVO0_MIN_ENV_SPAN;
    }

    normalized = (filtered_envelope - s_arm_emg_servo0.cfg.envelope_open) / span;
    normalized = clampf(normalized, 0.0f, 1.0f);
    grip_percent = normalized * 100.0f;

    if (grip_percent < s_arm_emg_servo0.cfg.grip_deadband_percent)
    {
        grip_percent = 0.0f;
    }

    return grip_percent;
}

void arm_link_init(void)
{
    Servo_Init_All();
    R_GPT_Open(g_timer4.p_ctrl, g_timer4.p_cfg);
    R_GPT_Start(g_timer4.p_ctrl);

    arm_reset_joint_speed_step();
    arm_reset_emg_servo0_config();
    arm_apply_emg_envelope_to_servo0(0.0f);
}

void arm_pose_calib_test(float base_deg, float shoulder_deg, float elbow_deg, float wrist_deg)
{
    float angle_code_5;
    float angle_code_4;
    float angle_code_3;
    float angle_code_2;
    float angle_code_1;

    base_deg = clampf(base_deg, 0.0f, 180.0f);
    shoulder_deg = clampf(shoulder_deg, 0.0f, 180.0f);
    elbow_deg = clampf(elbow_deg, 0.0f, 180.0f);
    wrist_deg = clampf(wrist_deg, 0.0f, 180.0f);

    angle_code_5 = apply_reverse_and_offset(base_deg, CODE_SERVO5_REVERSE, CODE_SERVO5_OFFSET);
    angle_code_5 = clampf(angle_code_5, 0.0f, 180.0f);

    map_shoulder_split(shoulder_deg, &angle_code_4, &angle_code_3);

    angle_code_2 = 90.0f - elbow_deg;
    angle_code_2 = apply_reverse_and_offset(angle_code_2, CODE_SERVO2_REVERSE, CODE_SERVO2_OFFSET);
    angle_code_2 = clampf(angle_code_2, 0.0f, 180.0f);

    angle_code_1 = 180.0f - wrist_deg;
    angle_code_1 = apply_reverse_and_offset(angle_code_1, CODE_SERVO1_REVERSE, CODE_SERVO1_OFFSET);
    angle_code_1 = clampf(angle_code_1, 0.0f, 180.0f);

    Servo_SetTargetAngle(5U, angle_code_5, s_arm_joint_speed_step);
    Servo_SetTargetAngle(4U, angle_code_4, s_arm_joint_speed_step);
    Servo_SetTargetAngle(3U, angle_code_3, s_arm_joint_speed_step);
    Servo_SetTargetAngle(2U, angle_code_2, s_arm_joint_speed_step);
    Servo_SetTargetAngle(1U, angle_code_1, s_arm_joint_speed_step);
}

void arm_apply_imu_pose_to_servos(const imu_servo_pose_t * p_pose)
{
    if (NULL == p_pose)
    {
        return;
    }

    arm_pose_calib_test((float) p_pose->hY_deg,
                        (float) p_pose->hZ_deg,
                        (float) p_pose->eZ_deg,
                        (float) p_pose->wX_deg);
}

void arm_apply_emg_envelope_to_servo0(float envelope)
{
    float target_grip_percent;
    float target_angle;
    float delta;

    arm_emg_servo0_ensure_initialized();

    s_arm_emg_servo0.filtered_envelope +=
        s_arm_emg_servo0.cfg.envelope_alpha * (envelope - s_arm_emg_servo0.filtered_envelope);

    target_grip_percent = arm_emg_servo0_compute_target_grip_percent(s_arm_emg_servo0.filtered_envelope);
    delta = target_grip_percent - s_arm_emg_servo0.grip_percent_f;

    if ((target_grip_percent <= 0.0f) &&
        (s_arm_emg_servo0.grip_percent_f <= s_arm_emg_servo0.cfg.grip_deadband_percent))
    {
        s_arm_emg_servo0.grip_percent_f = 0.0f;
    }
    else if (fabsf(delta) >= s_arm_emg_servo0.cfg.grip_hold_percent)
    {
        s_arm_emg_servo0.grip_percent_f += s_arm_emg_servo0.cfg.grip_alpha * delta;
        s_arm_emg_servo0.grip_percent_f = clampf(s_arm_emg_servo0.grip_percent_f, 0.0f, 100.0f);
    }

    target_angle = s_arm_emg_servo0.cfg.servo_open_angle +
                   ((s_arm_emg_servo0.cfg.servo_close_angle - s_arm_emg_servo0.cfg.servo_open_angle) *
                    (s_arm_emg_servo0.grip_percent_f / 100.0f));

    Servo_SetTargetAngle(0U, target_angle, s_arm_emg_servo0.cfg.servo_speed_step);
}

float arm_get_joint_speed_step(void)
{
    return s_arm_joint_speed_step;
}

void arm_reset_joint_speed_step(void)
{
    s_arm_joint_speed_step = ARM_SERVO_SPEED_STEP_DEFAULT;
}

fsp_err_t arm_set_joint_speed_step(float value)
{
    if (!isfinite(value) || (value <= 0.0f) || (value > 10.0f))
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    s_arm_joint_speed_step = value;
    return FSP_SUCCESS;
}

uint8_t arm_get_emg_grip_percent(void)
{
    arm_emg_servo0_ensure_initialized();
    return (uint8_t) (s_arm_emg_servo0.grip_percent_f + 0.5f);
}

float arm_get_emg_filtered_envelope(void)
{
    arm_emg_servo0_ensure_initialized();
    return s_arm_emg_servo0.filtered_envelope;
}

void arm_get_emg_servo0_config(arm_emg_servo0_cfg_t * p_cfg)
{
    arm_emg_servo0_ensure_initialized();

    if (NULL != p_cfg)
    {
        *p_cfg = s_arm_emg_servo0.cfg;
    }
}

void arm_reset_emg_servo0_config(void)
{
    arm_emg_servo0_load_defaults(&s_arm_emg_servo0.cfg);
    s_arm_emg_servo0.filtered_envelope = 0.0f;
    s_arm_emg_servo0.grip_percent_f = 0.0f;
    s_arm_emg_servo0.initialized = true;
}

fsp_err_t arm_set_emg_servo0_param(char const * p_name, float value)
{
    arm_emg_servo0_ensure_initialized();

    if ((NULL == p_name) || !isfinite(value))
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    if (0 == strcmp(p_name, "OPEN"))
    {
        if ((value < 0.0f) || (value >= (s_arm_emg_servo0.cfg.envelope_close - ARM_EMG_SERVO0_MIN_ENV_SPAN)))
        {
            return FSP_ERR_INVALID_ARGUMENT;
        }
        s_arm_emg_servo0.cfg.envelope_open = value;
    }
    else if (0 == strcmp(p_name, "CLOSE"))
    {
        if (value <= (s_arm_emg_servo0.cfg.envelope_open + ARM_EMG_SERVO0_MIN_ENV_SPAN))
        {
            return FSP_ERR_INVALID_ARGUMENT;
        }
        s_arm_emg_servo0.cfg.envelope_close = value;
    }
    else if (0 == strcmp(p_name, "ENV_ALPHA"))
    {
        if ((value <= 0.0f) || (value > 1.0f))
        {
            return FSP_ERR_INVALID_ARGUMENT;
        }
        s_arm_emg_servo0.cfg.envelope_alpha = value;
    }
    else if (0 == strcmp(p_name, "GRIP_ALPHA"))
    {
        if ((value <= 0.0f) || (value > 1.0f))
        {
            return FSP_ERR_INVALID_ARGUMENT;
        }
        s_arm_emg_servo0.cfg.grip_alpha = value;
    }
    else if (0 == strcmp(p_name, "DEADBAND"))
    {
        if ((value < 0.0f) || (value >= 100.0f))
        {
            return FSP_ERR_INVALID_ARGUMENT;
        }
        s_arm_emg_servo0.cfg.grip_deadband_percent = value;
    }
    else if (0 == strcmp(p_name, "HOLD"))
    {
        if ((value < 0.0f) || (value > 20.0f))
        {
            return FSP_ERR_INVALID_ARGUMENT;
        }
        s_arm_emg_servo0.cfg.grip_hold_percent = value;
    }
    else if (0 == strcmp(p_name, "SPEED"))
    {
        if ((value <= 0.0f) || (value > 10.0f))
        {
            return FSP_ERR_INVALID_ARGUMENT;
        }
        s_arm_emg_servo0.cfg.servo_speed_step = value;
    }
    else if (0 == strcmp(p_name, "OPEN_ANGLE"))
    {
        if ((value < 0.0f) || (value >= s_arm_emg_servo0.cfg.servo_close_angle))
        {
            return FSP_ERR_INVALID_ARGUMENT;
        }
        s_arm_emg_servo0.cfg.servo_open_angle = value;
    }
    else if (0 == strcmp(p_name, "CLOSE_ANGLE"))
    {
        if ((value > 180.0f) || (value <= s_arm_emg_servo0.cfg.servo_open_angle))
        {
            return FSP_ERR_INVALID_ARGUMENT;
        }
        s_arm_emg_servo0.cfg.servo_close_angle = value;
    }
    else
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    return FSP_SUCCESS;
}
