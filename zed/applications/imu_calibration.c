#include "imu_calibration.h"
#include "imu_math.h"
#include "imu_protocol.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int32_t          imu_clamp_int32(int32_t value, int32_t min_value, int32_t max_value);
static void             imu_set_flash_pattern(imu_app_context_t * p_ctx, uint8_t pulses, uint32_t now_us);
static bool             imu_capture_motion_components(imu_app_context_t * p_ctx, imu_motion_components_t * p_motion);
static imu_cal_result_t imu_learn_axis_map(imu_axis_map_t * p_map,
                                           Quaternion_t const * p_pose,
                                           imu_signal_source_t source,
                                           imu_angle_measure_t measure,
                                           int16_t center_deg,
                                           float target_delta_deg);
static bool             imu_measure_swing_deg(Quaternion_t const * p_pose,
                                              icm42688Float3_t const * p_reference,
                                              icm42688Float3_t const * p_axis,
                                              float * p_angle_deg);
static bool             imu_measure_twist_deg(Quaternion_t const * p_pose,
                                              icm42688Float3_t const * p_axis,
                                              float * p_angle_deg);
static bool             imu_solve_basis_coefficients(icm42688Float3_t const * p_basis1,
                                                     icm42688Float3_t const * p_basis2,
                                                     icm42688Float3_t const * p_value,
                                                     float * p_coeff1,
                                                     float * p_coeff2);
static bool             imu_finalize_upper_axis_maps(imu_app_context_t * p_ctx);
static bool             imu_finalize_lower_axis_maps(imu_app_context_t * p_ctx);
static imu_cal_result_t imu_record_current_step(imu_app_context_t * p_ctx, uint32_t now_us);
static bool             imu_apply_axis_map(imu_axis_map_t * p_map, float raw_deg, int32_t * p_output_deg);
static uint8_t          imu_get_grip_percent(void);

void imu_calibration_reset(imu_app_context_t * p_ctx)
{
    memset(&p_ctx->calibration, 0, sizeof(p_ctx->calibration));
    imu_quaternion_identity(&p_ctx->calibration.upper_offset);
    imu_quaternion_identity(&p_ctx->calibration.lower_offset);
    p_ctx->calibration.current_step = IMU_CAL_STEP_TPOSE;
    p_ctx->last_telemetry_time_us = 0U;
}

void imu_calibration_begin(imu_app_context_t * p_ctx, uint32_t now_us)
{
    imu_calibration_reset(p_ctx);
    p_ctx->calibration.last_button_time_us = now_us - IMU_BUTTON_DEBOUNCE_US;
    imu_set_flash_pattern(p_ctx, 1U, now_us);
    imu_protocol_send_cal_step(p_ctx);
}

void imu_calibration_handle_next(imu_app_context_t * p_ctx, uint32_t now_us)
{
    imu_cal_result_t result;
    imu_cal_step_t   finished_step;

    if (p_ctx->calibration.is_calibrated && (IMU_CAL_STEP_DONE == p_ctx->calibration.current_step))
    {
        imu_protocol_send_cal_done(p_ctx);
        return;
    }

    result = imu_record_current_step(p_ctx, now_us);
    finished_step = p_ctx->calibration.current_step;

    if (IMU_CAL_RESULT_OK != result)
    {
        imu_set_flash_pattern(p_ctx, 4U, now_us);
        imu_protocol_send_cal_error(p_ctx, result, finished_step);
        return;
    }

    imu_set_flash_pattern(p_ctx, 2U, now_us);
    imu_protocol_send_cal_ok(p_ctx, finished_step);

    if (IMU_CAL_STEP_WX == finished_step)
    {
        p_ctx->calibration.is_calibrated = p_ctx->calibration.hY_map.valid &&
                                           p_ctx->calibration.hZ_map.valid &&
                                           p_ctx->calibration.eZ_map.valid &&
                                           p_ctx->calibration.wX_map.valid;

        if (p_ctx->calibration.is_calibrated)
        {
            p_ctx->calibration.current_step = IMU_CAL_STEP_DONE;
            p_ctx->last_telemetry_time_us = 0U;
            imu_protocol_send_cal_done(p_ctx);
        }
        else
        {
            imu_protocol_send_cal_error(p_ctx, IMU_CAL_RESULT_AMBIG, finished_step);
        }
        return;
    }

    p_ctx->calibration.current_step = (imu_cal_step_t) (finished_step + 1);
    imu_protocol_send_cal_step(p_ctx);
}

bool imu_try_build_servo_pose(imu_app_context_t * p_ctx, imu_servo_pose_t * p_pose)
{
    imu_motion_components_t motion = {0};
    icm42688Float3_t        upper_current_vec = {0.0f, 0.0f, 0.0f};
    icm42688Float3_t        upper_hy_vec = {0.0f, 0.0f, 0.0f};
    icm42688Float3_t        upper_hz_vec = {0.0f, 0.0f, 0.0f};
    icm42688Float3_t        relative_current_vec = {0.0f, 0.0f, 0.0f};
    icm42688Float3_t        relative_ez_vec = {0.0f, 0.0f, 0.0f};
    icm42688Float3_t        relative_wx_vec = {0.0f, 0.0f, 0.0f};
    float                   hy_coeff = 0.0f;
    float                   hz_coeff = 0.0f;
    float                   ez_coeff = 0.0f;
    float                   wx_coeff = 0.0f;
    int32_t                 hY_deg;
    int32_t                 hZ_deg;
    int32_t                 eZ_deg;
    int32_t                 wX_deg;

    if ((NULL == p_pose) ||
        !p_ctx->calibration.is_calibrated ||
        !p_ctx->calibration.hY_map.valid ||
        !p_ctx->calibration.hZ_map.valid ||
        !p_ctx->calibration.eZ_map.valid ||
        !p_ctx->calibration.wX_map.valid)
    {
        return false;
    }

    if (!imu_capture_motion_components(p_ctx, &motion))
    {
        return false;
    }

    if (!imu_quaternion_extract_rotation_vector_deg(&motion.upper_bone, &upper_current_vec) ||
        !imu_quaternion_extract_rotation_vector_deg(&p_ctx->calibration.upper_hy_pose, &upper_hy_vec) ||
        !imu_quaternion_extract_rotation_vector_deg(&p_ctx->calibration.upper_hz_pose, &upper_hz_vec) ||
        !imu_quaternion_extract_rotation_vector_deg(&motion.relative_bone, &relative_current_vec) ||
        !imu_quaternion_extract_rotation_vector_deg(&p_ctx->calibration.relative_ez_pose, &relative_ez_vec) ||
        !imu_quaternion_extract_rotation_vector_deg(&p_ctx->calibration.relative_wx_pose, &relative_wx_vec))
    {
        return false;
    }

    if (!imu_solve_basis_coefficients(&upper_hy_vec, &upper_hz_vec, &upper_current_vec, &hy_coeff, &hz_coeff) ||
        !imu_solve_basis_coefficients(&relative_ez_vec, &relative_wx_vec, &relative_current_vec, &ez_coeff, &wx_coeff))
    {
        return false;
    }

    if (!imu_apply_axis_map(&p_ctx->calibration.hY_map, IMU_CAL_TARGET_DELTA_DEG * hy_coeff, &hY_deg) ||
        !imu_apply_axis_map(&p_ctx->calibration.hZ_map, -IMU_CAL_TARGET_DELTA_DEG * hz_coeff, &hZ_deg) ||
        !imu_apply_axis_map(&p_ctx->calibration.eZ_map, IMU_CAL_TARGET_DELTA_DEG * ez_coeff, &eZ_deg) ||
        !imu_apply_axis_map(&p_ctx->calibration.wX_map, IMU_CAL_TARGET_DELTA_DEG * wx_coeff, &wX_deg))
    {
        return false;
    }

    p_pose->hY_deg = (uint16_t) imu_clamp_int32(hY_deg, 0, 180);
    p_pose->hZ_deg = (uint16_t) imu_clamp_int32(hZ_deg, 0, 180);
    p_pose->eZ_deg = (uint16_t) imu_clamp_int32(eZ_deg, 0, 180);
    p_pose->wX_deg = (uint16_t) imu_clamp_int32(wX_deg, 0, 180);
    p_pose->grip_percent = (uint8_t) imu_clamp_int32((int32_t) imu_get_grip_percent(), 0, 100);

    return true;
}

static int32_t imu_clamp_int32(int32_t value, int32_t min_value, int32_t max_value)
{
    if (value < min_value)
    {
        return min_value;
    }

    if (value > max_value)
    {
        return max_value;
    }

    return value;
}

static void imu_set_flash_pattern(imu_app_context_t * p_ctx, uint8_t pulses, uint32_t now_us)
{
    p_ctx->calibration.led_flash_active = (pulses > 0U);
    p_ctx->calibration.led_flash_pulses = pulses;
    p_ctx->calibration.led_flash_start_us = now_us;
}

static bool imu_capture_motion_components(imu_app_context_t * p_ctx, imu_motion_components_t * p_motion)
{
    Quaternion_t upper_bone;
    Quaternion_t lower_bone;
    Quaternion_t upper_bone_inverse;
    Quaternion_t relative_bone;

    if ((NULL == p_motion) ||
        (0U == p_ctx->upper_imu.last_sample_time_us) ||
        (0U == p_ctx->lower_imu.last_sample_time_us))
    {
        return false;
    }

    upper_bone = imu_quaternion_multiply(&p_ctx->upper_imu.quat, &p_ctx->calibration.upper_offset);
    lower_bone = imu_quaternion_multiply(&p_ctx->lower_imu.quat, &p_ctx->calibration.lower_offset);
    upper_bone_inverse = imu_quaternion_inverse(&upper_bone);
    relative_bone = imu_quaternion_multiply(&upper_bone_inverse, &lower_bone);

    p_motion->upper_bone = upper_bone;
    p_motion->relative_bone = relative_bone;

    return true;
}

static imu_cal_result_t imu_learn_axis_map(imu_axis_map_t * p_map,
                                           Quaternion_t const * p_pose,
                                           imu_signal_source_t source,
                                           imu_angle_measure_t measure,
                                           int16_t center_deg,
                                           float target_delta_deg)
{
    icm42688Float3_t axis = {0.0f, 0.0f, 0.0f};
    float            raw_angle_deg = 0.0f;

    if ((NULL == p_map) || (NULL == p_pose))
    {
        return IMU_CAL_RESULT_AMBIG;
    }

    if (!imu_quaternion_extract_axis_angle(p_pose, &axis, &raw_angle_deg))
    {
        return IMU_CAL_RESULT_AMBIG;
    }

    if (raw_angle_deg < IMU_AXIS_MIN_RESPONSE_DEG)
    {
        return IMU_CAL_RESULT_WEAK;
    }

    p_map->source = source;
    p_map->measure = measure;
    p_map->axis = axis;
    p_map->filtered_raw_deg = 0.0f;
    p_map->gain = target_delta_deg / raw_angle_deg;
    p_map->center_deg = center_deg;
    p_map->last_output_deg = center_deg;
    p_map->valid = true;
    p_map->has_filtered_raw = false;
    p_map->has_reference = false;
    p_map->has_last_raw = false;
    p_map->has_last_output = false;
    p_map->last_raw_deg = 0.0f;

    return IMU_CAL_RESULT_OK;
}

static bool imu_measure_swing_deg(Quaternion_t const * p_pose,
                                  icm42688Float3_t const * p_reference,
                                  icm42688Float3_t const * p_axis,
                                  float * p_angle_deg)
{
    icm42688Float3_t ref_projected;
    icm42688Float3_t current_vector;
    icm42688Float3_t current_projected;
    icm42688Float3_t cross_vector;
    float            sin_term;
    float            cos_term;

    if ((NULL == p_pose) || (NULL == p_reference) || (NULL == p_axis) || (NULL == p_angle_deg))
    {
        return false;
    }

    ref_projected.x = p_reference->x - (p_axis->x * imu_vector_dot(p_reference, p_axis));
    ref_projected.y = p_reference->y - (p_axis->y * imu_vector_dot(p_reference, p_axis));
    ref_projected.z = p_reference->z - (p_axis->z * imu_vector_dot(p_reference, p_axis));
    if (!imu_vector_normalize_unit(&ref_projected))
    {
        return false;
    }

    current_vector = imu_quaternion_rotate_vector(p_pose, p_reference);
    current_projected.x = current_vector.x - (p_axis->x * imu_vector_dot(&current_vector, p_axis));
    current_projected.y = current_vector.y - (p_axis->y * imu_vector_dot(&current_vector, p_axis));
    current_projected.z = current_vector.z - (p_axis->z * imu_vector_dot(&current_vector, p_axis));
    if (!imu_vector_normalize_unit(&current_projected))
    {
        return false;
    }

    cross_vector = imu_vector_cross(&ref_projected, &current_projected);
    sin_term = imu_vector_dot(p_axis, &cross_vector);
    cos_term = imu_clampf(imu_vector_dot(&ref_projected, &current_projected), -1.0f, 1.0f);
    *p_angle_deg = atan2f(sin_term, cos_term) * IMU_RAD_TO_DEG;

    return true;
}

static bool imu_measure_twist_deg(Quaternion_t const * p_pose,
                                  icm42688Float3_t const * p_axis,
                                  float * p_angle_deg)
{
    Quaternion_t     pose = {0.0f, 0.0f, 0.0f, 0.0f};
    Quaternion_t     twist = {0.0f, 0.0f, 0.0f, 0.0f};
    icm42688Float3_t quaternion_vector = {0.0f, 0.0f, 0.0f};
    float            projection;
    float            twist_vector_norm;
    float            angle_deg;

    if ((NULL == p_pose) || (NULL == p_axis) || (NULL == p_angle_deg))
    {
        return false;
    }

    pose = *p_pose;
    imu_quaternion_normalize(&pose);
    if (pose.q0 < 0.0f)
    {
        pose.q0 = -pose.q0;
        pose.q1 = -pose.q1;
        pose.q2 = -pose.q2;
        pose.q3 = -pose.q3;
    }

    quaternion_vector.x = pose.q1;
    quaternion_vector.y = pose.q2;
    quaternion_vector.z = pose.q3;
    projection = imu_vector_dot(&quaternion_vector, p_axis);

    twist.q0 = pose.q0;
    twist.q1 = p_axis->x * projection;
    twist.q2 = p_axis->y * projection;
    twist.q3 = p_axis->z * projection;
    imu_quaternion_normalize(&twist);
    if (twist.q0 < 0.0f)
    {
        twist.q0 = -twist.q0;
        twist.q1 = -twist.q1;
        twist.q2 = -twist.q2;
        twist.q3 = -twist.q3;
    }

    twist_vector_norm = sqrtf((twist.q1 * twist.q1) + (twist.q2 * twist.q2) + (twist.q3 * twist.q3));
    angle_deg = (2.0f * atan2f(twist_vector_norm, twist.q0)) * IMU_RAD_TO_DEG;
    if (projection < 0.0f)
    {
        angle_deg = -angle_deg;
    }

    *p_angle_deg = angle_deg;

    return true;
}

static bool imu_solve_basis_coefficients(icm42688Float3_t const * p_basis1,
                                         icm42688Float3_t const * p_basis2,
                                         icm42688Float3_t const * p_value,
                                         float * p_coeff1,
                                         float * p_coeff2)
{
    float a11;
    float a12;
    float a22;
    float b1;
    float b2;
    float det;

    if ((NULL == p_basis1) || (NULL == p_basis2) || (NULL == p_value) || (NULL == p_coeff1) || (NULL == p_coeff2))
    {
        return false;
    }

    a11 = imu_vector_dot(p_basis1, p_basis1);
    a12 = imu_vector_dot(p_basis1, p_basis2);
    a22 = imu_vector_dot(p_basis2, p_basis2);
    det = (a11 * a22) - (a12 * a12);
    if (fabsf(det) <= IMU_VECTOR_EPSILON)
    {
        return false;
    }

    b1 = imu_vector_dot(p_value, p_basis1);
    b2 = imu_vector_dot(p_value, p_basis2);
    *p_coeff1 = ((b1 * a22) - (b2 * a12)) / det;
    *p_coeff2 = ((a11 * b2) - (a12 * b1)) / det;

    return true;
}

static bool imu_finalize_upper_axis_maps(imu_app_context_t * p_ctx)
{
    float          best_score = 1.0e9f;
    bool           found = false;
    imu_axis_map_t hy_map = p_ctx->calibration.hY_map;
    imu_axis_map_t hz_map = p_ctx->calibration.hZ_map;

    for (int hy_sign = -1; hy_sign <= 1; hy_sign += 2)
    {
        for (int hz_sign = -1; hz_sign <= 1; hz_sign += 2)
        {
            icm42688Float3_t axis_hy = {hy_map.axis.x * (float) hy_sign,
                                        hy_map.axis.y * (float) hy_sign,
                                        hy_map.axis.z * (float) hy_sign};
            icm42688Float3_t axis_hz = {hz_map.axis.x * (float) hz_sign,
                                        hz_map.axis.y * (float) hz_sign,
                                        hz_map.axis.z * (float) hz_sign};
            icm42688Float3_t reference = imu_vector_cross(&axis_hy, &axis_hz);
            float            hy_raw_deg = 0.0f;
            float            hz_raw_deg = 0.0f;
            float            score;

            if (!imu_vector_normalize_unit(&reference))
            {
                continue;
            }

            if (!imu_measure_swing_deg(&p_ctx->calibration.upper_hy_pose, &reference, &axis_hy, &hy_raw_deg) ||
                !imu_measure_swing_deg(&p_ctx->calibration.upper_hz_pose, &reference, &axis_hz, &hz_raw_deg))
            {
                continue;
            }

            if ((fabsf(hy_raw_deg) < IMU_AXIS_MIN_RESPONSE_DEG) ||
                (fabsf(hz_raw_deg) < IMU_AXIS_MIN_RESPONSE_DEG))
            {
                continue;
            }

            score = fabsf(IMU_CAL_TARGET_DELTA_DEG - hy_raw_deg) +
                    fabsf((-IMU_CAL_TARGET_DELTA_DEG) - hz_raw_deg);
            if ((!found) || (score < best_score))
            {
                found = true;
                best_score = score;
                p_ctx->calibration.hY_map.axis = axis_hy;
                p_ctx->calibration.hY_map.reference = reference;
                p_ctx->calibration.hY_map.gain = IMU_CAL_TARGET_DELTA_DEG / hy_raw_deg;
                p_ctx->calibration.hY_map.has_reference = true;
                p_ctx->calibration.hY_map.has_filtered_raw = false;
                p_ctx->calibration.hY_map.has_last_raw = false;
                p_ctx->calibration.hY_map.has_last_output = false;

                p_ctx->calibration.hZ_map.axis = axis_hz;
                p_ctx->calibration.hZ_map.reference = reference;
                p_ctx->calibration.hZ_map.gain = (-IMU_CAL_TARGET_DELTA_DEG) / hz_raw_deg;
                p_ctx->calibration.hZ_map.has_reference = true;
                p_ctx->calibration.hZ_map.has_filtered_raw = false;
                p_ctx->calibration.hZ_map.has_last_raw = false;
                p_ctx->calibration.hZ_map.has_last_output = false;
            }
        }
    }

    return found;
}

static bool imu_finalize_lower_axis_maps(imu_app_context_t * p_ctx)
{
    float          best_score = 1.0e9f;
    bool           found = false;
    imu_axis_map_t ez_map = p_ctx->calibration.eZ_map;
    imu_axis_map_t wx_map = p_ctx->calibration.wX_map;

    for (int ez_sign = -1; ez_sign <= 1; ez_sign += 2)
    {
        for (int wx_sign = -1; wx_sign <= 1; wx_sign += 2)
        {
            icm42688Float3_t axis_ez = {ez_map.axis.x * (float) ez_sign,
                                        ez_map.axis.y * (float) ez_sign,
                                        ez_map.axis.z * (float) ez_sign};
            icm42688Float3_t axis_wx = {wx_map.axis.x * (float) wx_sign,
                                        wx_map.axis.y * (float) wx_sign,
                                        wx_map.axis.z * (float) wx_sign};
            float            ez_raw_deg = 0.0f;
            float            wx_raw_deg = 0.0f;
            float            score;

            if (!imu_vector_normalize_unit(&axis_wx))
            {
                continue;
            }

            if (!imu_measure_swing_deg(&p_ctx->calibration.relative_ez_pose, &axis_wx, &axis_ez, &ez_raw_deg) ||
                !imu_measure_twist_deg(&p_ctx->calibration.relative_wx_pose, &axis_wx, &wx_raw_deg))
            {
                continue;
            }

            if ((fabsf(ez_raw_deg) < IMU_AXIS_MIN_RESPONSE_DEG) ||
                (fabsf(wx_raw_deg) < IMU_AXIS_MIN_RESPONSE_DEG))
            {
                continue;
            }

            score = fabsf(IMU_CAL_TARGET_DELTA_DEG - ez_raw_deg) +
                    fabsf(IMU_CAL_TARGET_DELTA_DEG - wx_raw_deg);
            if ((!found) || (score < best_score))
            {
                found = true;
                best_score = score;
                p_ctx->calibration.eZ_map.axis = axis_ez;
                p_ctx->calibration.eZ_map.reference = axis_wx;
                p_ctx->calibration.eZ_map.gain = IMU_CAL_TARGET_DELTA_DEG / ez_raw_deg;
                p_ctx->calibration.eZ_map.has_reference = true;
                p_ctx->calibration.eZ_map.has_filtered_raw = false;
                p_ctx->calibration.eZ_map.has_last_raw = false;
                p_ctx->calibration.eZ_map.has_last_output = false;

                p_ctx->calibration.wX_map.axis = axis_wx;
                p_ctx->calibration.wX_map.reference = axis_wx;
                p_ctx->calibration.wX_map.gain = IMU_CAL_TARGET_DELTA_DEG / wx_raw_deg;
                p_ctx->calibration.wX_map.has_reference = true;
                p_ctx->calibration.wX_map.has_filtered_raw = false;
                p_ctx->calibration.wX_map.has_last_raw = false;
                p_ctx->calibration.wX_map.has_last_output = false;
            }
        }
    }

    return found;
}

static imu_cal_result_t imu_record_current_step(imu_app_context_t * p_ctx, uint32_t now_us)
{
    imu_motion_components_t motion = {0};

    (void) now_us;

    if ((0U == p_ctx->upper_imu.last_sample_time_us) || (0U == p_ctx->lower_imu.last_sample_time_us))
    {
        return IMU_CAL_RESULT_NODATA;
    }

    if (IMU_CAL_STEP_TPOSE == p_ctx->calibration.current_step)
    {
        p_ctx->calibration.upper_offset = imu_quaternion_inverse(&p_ctx->upper_imu.quat);
        p_ctx->calibration.lower_offset = imu_quaternion_inverse(&p_ctx->lower_imu.quat);
        p_ctx->calibration.hY_map.valid = false;
        p_ctx->calibration.hZ_map.valid = false;
        p_ctx->calibration.eZ_map.valid = false;
        p_ctx->calibration.wX_map.valid = false;
        p_ctx->calibration.is_calibrated = false;
        p_ctx->last_telemetry_time_us = 0U;
        return IMU_CAL_RESULT_OK;
    }

    if (!imu_capture_motion_components(p_ctx, &motion))
    {
        return IMU_CAL_RESULT_NODATA;
    }

    switch (p_ctx->calibration.current_step)
    {
        case IMU_CAL_STEP_HY:
        {
            imu_cal_result_t result = imu_learn_axis_map(&p_ctx->calibration.hY_map,
                                                         &motion.upper_bone,
                                                         IMU_SIGNAL_SOURCE_UPPER,
                                                         IMU_ANGLE_MEASURE_SWING,
                                                         90,
                                                         IMU_CAL_TARGET_DELTA_DEG);
            if (IMU_CAL_RESULT_OK == result)
            {
                p_ctx->calibration.upper_hy_pose = motion.upper_bone;
            }
            return result;
        }

        case IMU_CAL_STEP_HZ:
        {
            imu_cal_result_t result = imu_learn_axis_map(&p_ctx->calibration.hZ_map,
                                                         &motion.upper_bone,
                                                         IMU_SIGNAL_SOURCE_UPPER,
                                                         IMU_ANGLE_MEASURE_SWING,
                                                         90,
                                                         -IMU_CAL_TARGET_DELTA_DEG);
            if (IMU_CAL_RESULT_OK != result)
            {
                return result;
            }

            p_ctx->calibration.upper_hz_pose = motion.upper_bone;
            return imu_finalize_upper_axis_maps(p_ctx) ? IMU_CAL_RESULT_OK : IMU_CAL_RESULT_AMBIG;
        }

        case IMU_CAL_STEP_EZ:
        {
            imu_cal_result_t result = imu_learn_axis_map(&p_ctx->calibration.eZ_map,
                                                         &motion.relative_bone,
                                                         IMU_SIGNAL_SOURCE_RELATIVE,
                                                         IMU_ANGLE_MEASURE_SWING,
                                                         0,
                                                         IMU_CAL_TARGET_DELTA_DEG);
            if (IMU_CAL_RESULT_OK == result)
            {
                p_ctx->calibration.relative_ez_pose = motion.relative_bone;
            }
            return result;
        }

        case IMU_CAL_STEP_WX:
        {
            imu_cal_result_t result = imu_learn_axis_map(&p_ctx->calibration.wX_map,
                                                         &motion.relative_bone,
                                                         IMU_SIGNAL_SOURCE_RELATIVE,
                                                         IMU_ANGLE_MEASURE_TWIST,
                                                         90,
                                                         IMU_CAL_TARGET_DELTA_DEG);
            if (IMU_CAL_RESULT_OK != result)
            {
                return result;
            }

            p_ctx->calibration.relative_wx_pose = motion.relative_bone;
            return imu_finalize_lower_axis_maps(p_ctx) ? IMU_CAL_RESULT_OK : IMU_CAL_RESULT_AMBIG;
        }

        default:
            return IMU_CAL_RESULT_AMBIG;
    }
}

static bool imu_apply_axis_map(imu_axis_map_t * p_map, float raw_deg, int32_t * p_output_deg)
{
    float   delta_deg;
    int32_t output_deg;

    if ((NULL == p_map) || (NULL == p_output_deg) || !p_map->valid)
    {
        return false;
    }

    if (p_map->has_last_raw)
    {
        delta_deg = imu_clampf(raw_deg - p_map->last_raw_deg, -IMU_RUNTIME_MAX_STEP_DEG, IMU_RUNTIME_MAX_STEP_DEG);
        raw_deg = p_map->last_raw_deg + delta_deg;
    }

    p_map->last_raw_deg = raw_deg;
    p_map->has_last_raw = true;

    if (p_map->has_filtered_raw)
    {
        raw_deg = p_map->filtered_raw_deg +
                  (IMU_RUNTIME_RAW_FILTER_ALPHA * (raw_deg - p_map->filtered_raw_deg));
    }

    p_map->filtered_raw_deg = raw_deg;
    p_map->has_filtered_raw = true;

    output_deg = (int32_t) roundf((float) p_map->center_deg + raw_deg);
    if (p_map->has_last_output &&
        (abs(output_deg - (int32_t) p_map->last_output_deg) <= IMU_RUNTIME_OUTPUT_DEADBAND))
    {
        output_deg = (int32_t) p_map->last_output_deg;
    }

    p_map->last_output_deg = (int16_t) output_deg;
    p_map->has_last_output = true;
    *p_output_deg = output_deg;

    return true;
}

static uint8_t imu_get_grip_percent(void)
{
    return 0U;
}
