#include "imu_calibration.h"
#include "app_arm_link.h"
#include "emg_runtime.h"
#include "imu_math.h"
#include "imu_protocol.h"
#include "imu_runtime.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define IMU_FINAL_DRIFT_SAMPLES          200U
#define IMU_FINAL_DRIFT_MAX_ATTEMPTS     400U
#define IMU_FINAL_DRIFT_ACC_MIN_G        0.94f
#define IMU_FINAL_DRIFT_ACC_MAX_G        1.06f
#define IMU_FINAL_DRIFT_GYRO_MAX_RAD_S   (2.5f * IMU_DEG_TO_RAD)
#define IMU_FK_OPT_RESEED_THRESHOLD_DEG     2.0f
#define IMU_RUNTIME_POSE_RESIDUAL_HOLD_DEG  25.0f
#define IMU_RUNTIME_LOWER_RESIDUAL_HOLD_DEG 35.0f

/*
 * 标定模块总览
 *
 * 这套算法的核心思路不是“读取一个姿态角直接映射到舵机”，
 * 而是先学习每个机械臂自由度在 IMU 姿态空间中的代表性方向，
 * 再在运行时把当前姿态分解到这些代表性方向上。
 *
 * 因此它本质上是一套“基于标定样本的姿态投影映射”：
 * - 标定阶段：学习动作基向量和缩放关系；
 * - 运行阶段：把实时姿态投影到这些基向量上。
 */

/* 标定模块的任务是学习“人体/IMU 的动作”如何映射到机械臂支持的几个舵机自由度。 */
static int32_t          imu_clamp_int32(int32_t value, int32_t min_value, int32_t max_value);
static void             imu_set_flash_pattern(imu_app_context_t * p_ctx, uint8_t pulses, uint32_t now_us);
static void             imu_reset_fk_warm_start(imu_app_context_t * p_ctx);
static bool             imu_capture_motion_components(imu_app_context_t * p_ctx, imu_motion_components_t * p_motion);
static imu_cal_result_t imu_learn_axis_map(imu_axis_map_t * p_map,
                                           Quaternion_t const * p_pose,
                                           imu_signal_source_t source,
                                           imu_angle_measure_t measure,
                                           int16_t center_deg,
                                           float target_delta_deg);
static float            imu_measure_axis_separation_deg(icm42688Float3_t const * p_axis1,
                                                        icm42688Float3_t const * p_axis2);
static bool             imu_quality_metrics_pass(imu_cal_quality_metrics_t const * p_metrics,
                                                 float primary_target_1_deg,
                                                 float primary_target_2_deg);
static bool             imu_measure_swing_deg(Quaternion_t const * p_pose,
                                              icm42688Float3_t const * p_reference,
                                              icm42688Float3_t const * p_axis,
                                              float * p_angle_deg);
static bool             imu_measure_twist_deg(Quaternion_t const * p_pose,
                                              icm42688Float3_t const * p_axis,
                                              float * p_angle_deg);
static bool             imu_measure_axis_map_raw_deg(imu_axis_map_t const * p_map,
                                                     imu_motion_components_t const * p_motion,
                                                     float * p_raw_deg);
static bool             imu_calc_pose_basis_residual_deg(Quaternion_t const * p_pose,
                                                         Quaternion_t const * p_basis1_pose,
                                                         Quaternion_t const * p_basis2_pose,
                                                         float * p_residual_deg);
static bool             imu_get_stable_axis_output(imu_axis_map_t const * p_map,
                                                   int32_t * p_output_deg);
static bool             imu_try_build_upper_servo_pose_direct(imu_app_context_t * p_ctx,
                                                              imu_motion_components_t const * p_motion,
                                                              int32_t * p_hY_deg,
                                                              int32_t * p_hZ_deg);
static bool             imu_try_build_lower_servo_pose_hybrid(imu_app_context_t * p_ctx,
                                                              imu_motion_components_t const * p_motion,
                                                              int32_t * p_eZ_deg,
                                                              int32_t * p_wX_deg);
static bool             imu_build_pose_from_axis_maps(imu_axis_map_t const * p_map1,
                                                      imu_axis_map_t const * p_map2,
                                                      float primary_deg,
                                                      float secondary_deg,
                                                      Quaternion_t * p_pose);
static bool             imu_evaluate_fk_pair_cost_deg(imu_axis_map_t const * p_map1,
                                                      imu_axis_map_t const * p_map2,
                                                      Quaternion_t const * p_target_pose,
                                                      float primary_deg,
                                                      float secondary_deg,
                                                      float * p_cost_deg);
static bool             imu_optimize_joint_pair(imu_axis_map_t const * p_map1,
                                                imu_axis_map_t const * p_map2,
                                                Quaternion_t const * p_target_pose,
                                                imu_fk_pair_state_t * p_state,
                                                float center_primary_deg,
                                                float center_secondary_deg,
                                                float * p_best_primary_deg,
                                                float * p_best_secondary_deg,
                                                float * p_best_cost_deg);
static bool             imu_try_build_servo_pose_fk_opt(imu_app_context_t * p_ctx,
                                                        imu_motion_components_t const * p_motion,
                                                        int32_t * p_hY_deg,
                                                        int32_t * p_hZ_deg,
                                                        int32_t * p_eZ_deg,
                                                        int32_t * p_wX_deg);
static bool             imu_finalize_upper_axis_maps(imu_app_context_t * p_ctx);
static bool             imu_finalize_lower_axis_maps(imu_app_context_t * p_ctx);
static imu_cal_result_t imu_record_current_step(imu_app_context_t * p_ctx, uint32_t now_us);
static void             imu_refine_zero_drift_after_final_step(imu_app_context_t * p_ctx);
static bool             imu_apply_axis_output_common(imu_axis_map_t * p_map,
                                                     float raw_deg,
                                                     bool wrap_delta,
                                                     int32_t * p_output_deg);
static bool             imu_apply_axis_map(imu_axis_map_t * p_map, float raw_deg, int32_t * p_output_deg);
static bool             imu_apply_servo_target_deg(imu_axis_map_t * p_map, float target_deg, int32_t * p_output_deg);
static uint8_t          imu_get_grip_percent(void);

void imu_calibration_reset(imu_app_context_t * p_ctx)
{
    /* 清空所有已学习的轴映射，并把姿态偏置恢复成单位姿态参考。 */
    memset(&p_ctx->calibration, 0, sizeof(p_ctx->calibration));
    imu_quaternion_identity(&p_ctx->calibration.upper_offset);
    imu_quaternion_identity(&p_ctx->calibration.lower_offset);
    imu_reset_fk_warm_start(p_ctx);
    p_ctx->calibration.current_step = IMU_CAL_STEP_TPOSE;
    p_ctx->last_telemetry_time_us = 0U;
}

void imu_calibration_begin(imu_app_context_t * p_ctx, uint32_t now_us)
{
    /* 重新开始引导式标定，同时通过串口告知外部当前应执行的第一步动作。 */
    imu_calibration_reset(p_ctx);
    p_ctx->calibration.last_button_time_us = now_us - IMU_BUTTON_DEBOUNCE_US;
    imu_set_flash_pattern(p_ctx, 1U, now_us);
    imu_protocol_send_cal_step(p_ctx);
}

void imu_calibration_handle_next(imu_app_context_t * p_ctx, uint32_t now_us)
{
    /* 记录当前姿态并校验它是否足够清晰、可用于学习当前自由度；成功后推进下一步。 */
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
    /* Main runtime mapping: IMU pose -> calibrated servo pose. */
    /*
     * 运行时姿态映射主入口。
     *
     * 这里会先构造上臂姿态和前臂相对姿态，
     * 再把它们转换成旋转向量，
     * 然后把当前旋转向量分解到标定阶段学到的基向量上，
     * 最终生成 hY / hZ / eZ / wX 四个舵机角。
     */
    /* 把当前骨段姿态投影到已学习到的运动基底上，得到每个舵机的目标输出角。 */
    imu_motion_components_t motion = {0};
    int32_t                 hY_deg;
    int32_t                 hZ_deg;
    int32_t                 eZ_deg;
    int32_t                 wX_deg;

    if ((NULL == p_ctx) ||
        (NULL == p_pose) ||
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

    if ((!imu_try_build_upper_servo_pose_direct(p_ctx, &motion, &hY_deg, &hZ_deg) ||
         !imu_try_build_lower_servo_pose_hybrid(p_ctx, &motion, &eZ_deg, &wX_deg)) &&
        !imu_try_build_servo_pose_fk_opt(p_ctx, &motion, &hY_deg, &hZ_deg, &eZ_deg, &wX_deg))
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

static void imu_reset_fk_warm_start(imu_app_context_t * p_ctx)
{
    if (NULL == p_ctx)
    {
        return;
    }

    p_ctx->calibration.upper_fk_state.last_primary_deg = 90.0f;
    p_ctx->calibration.upper_fk_state.last_secondary_deg = 90.0f;
    p_ctx->calibration.upper_fk_state.initialized = false;

    p_ctx->calibration.lower_fk_state.last_primary_deg = 0.0f;
    p_ctx->calibration.lower_fk_state.last_secondary_deg = 90.0f;
    p_ctx->calibration.lower_fk_state.initialized = false;
}

static void imu_set_flash_pattern(imu_app_context_t * p_ctx, uint8_t pulses, uint32_t now_us)
{
    /* 用闪灯次数表达当前反馈状态，这样即使没连串口也能看到标定提示。 */
    p_ctx->calibration.led_flash_active = (pulses > 0U);
    p_ctx->calibration.led_flash_pulses = pulses;
    p_ctx->calibration.led_flash_start_us = now_us;
}

static bool imu_capture_motion_components(imu_app_context_t * p_ctx, imu_motion_components_t * p_motion)
{
    /*
     * 这一层把原始 IMU 姿态转换成骨段姿态：
     * - upper_bone：上臂骨段姿态；
     * - lower_bone：下臂骨段姿态；
     * - relative_bone：下臂相对于上臂的姿态。
     *
     * relative_bone 是后续肘部/腕部自由度映射的关键输入。
     */
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

    /* 先把 T 姿态记录下来的偏置应用进去，把两个 IMU 都转换到机械臂模型坐标系。 */
    upper_bone = imu_quaternion_multiply(&p_ctx->upper_imu.quat, &p_ctx->calibration.upper_offset);
    lower_bone = imu_quaternion_multiply(&p_ctx->lower_imu.quat, &p_ctx->calibration.lower_offset);
    /* 再把下臂姿态转换到上臂坐标系下，后续肘部和腕部映射都依赖这个相对姿态。 */
    upper_bone_inverse = imu_quaternion_inverse(&upper_bone);
    relative_bone = imu_quaternion_multiply(&upper_bone_inverse, &lower_bone);

    p_motion->upper_bone = upper_bone;
    p_motion->relative_bone = relative_bone;

    return true;
}

static bool imu_build_pose_from_axis_maps(imu_axis_map_t const * p_map1,
                                          imu_axis_map_t const * p_map2,
                                          float primary_deg,
                                          float secondary_deg,
                                          Quaternion_t * p_pose)
{
    Quaternion_t q_primary;
    Quaternion_t q_secondary;
    float        primary_raw_deg;
    float        secondary_raw_deg;

    if ((NULL == p_map1) || (NULL == p_map2) || (NULL == p_pose) || !p_map1->valid || !p_map2->valid)
    {
        return false;
    }

    if ((fabsf(p_map1->gain) <= IMU_VECTOR_EPSILON) || (fabsf(p_map2->gain) <= IMU_VECTOR_EPSILON))
    {
        return false;
    }

    primary_deg = imu_clampf(primary_deg, 0.0f, 180.0f);
    secondary_deg = imu_clampf(secondary_deg, 0.0f, 180.0f);
    primary_raw_deg = (primary_deg - (float) p_map1->center_deg) / p_map1->gain;
    secondary_raw_deg = (secondary_deg - (float) p_map2->center_deg) / p_map2->gain;

    q_primary = imu_quaternion_from_axis_angle_deg(&p_map1->axis, primary_raw_deg);
    q_secondary = imu_quaternion_from_axis_angle_deg(&p_map2->axis, secondary_raw_deg);
    *p_pose = imu_quaternion_multiply(&q_primary, &q_secondary);

    return true;
}

static bool imu_evaluate_fk_pair_cost_deg(imu_axis_map_t const * p_map1,
                                          imu_axis_map_t const * p_map2,
                                          Quaternion_t const * p_target_pose,
                                          float primary_deg,
                                          float secondary_deg,
                                          float * p_cost_deg)
{
    Quaternion_t predicted_pose;

    if ((NULL == p_target_pose) || (NULL == p_cost_deg))
    {
        return false;
    }

    if (!imu_build_pose_from_axis_maps(p_map1, p_map2, primary_deg, secondary_deg, &predicted_pose))
    {
        return false;
    }

    *p_cost_deg = imu_quaternion_angular_distance_deg(&predicted_pose, p_target_pose);
    return true;
}

static bool imu_optimize_joint_pair(imu_axis_map_t const * p_map1,
                                    imu_axis_map_t const * p_map2,
                                    Quaternion_t const * p_target_pose,
                                    imu_fk_pair_state_t * p_state,
                                    float center_primary_deg,
                                    float center_secondary_deg,
                                    float * p_best_primary_deg,
                                    float * p_best_secondary_deg,
                                    float * p_best_cost_deg)
{
    static float const coarse_grid_deg[] = {0.0f, 45.0f, 90.0f, 135.0f, 180.0f};
    static float const step_sizes_deg[] = {30.0f, 12.0f, 4.0f, 1.0f, 0.25f};
    float             best_primary_deg;
    float             best_secondary_deg;
    float             best_cost_deg;

    if ((NULL == p_target_pose) || (NULL == p_state) ||
        (NULL == p_best_primary_deg) || (NULL == p_best_secondary_deg) || (NULL == p_best_cost_deg))
    {
        return false;
    }

    if (p_state->initialized)
    {
        best_primary_deg = p_state->last_primary_deg;
        best_secondary_deg = p_state->last_secondary_deg;
    }
    else
    {
        best_primary_deg = center_primary_deg;
        best_secondary_deg = center_secondary_deg;
    }

    if (!imu_evaluate_fk_pair_cost_deg(p_map1,
                                       p_map2,
                                       p_target_pose,
                                       best_primary_deg,
                                       best_secondary_deg,
                                       &best_cost_deg))
    {
        return false;
    }

    if (best_cost_deg > IMU_FK_OPT_RESEED_THRESHOLD_DEG)
    {
        for (uint32_t i = 0U; i < (sizeof(coarse_grid_deg) / sizeof(coarse_grid_deg[0])); i++)
        {
            for (uint32_t j = 0U; j < (sizeof(coarse_grid_deg) / sizeof(coarse_grid_deg[0])); j++)
            {
                float coarse_cost_deg = 0.0f;

                if (!imu_evaluate_fk_pair_cost_deg(p_map1,
                                                   p_map2,
                                                   p_target_pose,
                                                   coarse_grid_deg[i],
                                                   coarse_grid_deg[j],
                                                   &coarse_cost_deg))
                {
                    return false;
                }

                if (coarse_cost_deg < best_cost_deg)
                {
                    best_primary_deg = coarse_grid_deg[i];
                    best_secondary_deg = coarse_grid_deg[j];
                    best_cost_deg = coarse_cost_deg;
                }
            }
        }
    }

    for (uint32_t step_index = 0U; step_index < (sizeof(step_sizes_deg) / sizeof(step_sizes_deg[0])); step_index++)
    {
        bool improved = true;

        while (improved)
        {
            improved = false;

            for (int primary_dir = -1; primary_dir <= 1; primary_dir++)
            {
                for (int secondary_dir = -1; secondary_dir <= 1; secondary_dir++)
                {
                    float candidate_primary_deg;
                    float candidate_secondary_deg;
                    float candidate_cost_deg = 0.0f;

                    if ((0 == primary_dir) && (0 == secondary_dir))
                    {
                        continue;
                    }

                    candidate_primary_deg = imu_clampf(best_primary_deg + ((float) primary_dir * step_sizes_deg[step_index]),
                                                       0.0f,
                                                       180.0f);
                    candidate_secondary_deg = imu_clampf(best_secondary_deg +
                                                         ((float) secondary_dir * step_sizes_deg[step_index]),
                                                         0.0f,
                                                         180.0f);

                    if (!imu_evaluate_fk_pair_cost_deg(p_map1,
                                                       p_map2,
                                                       p_target_pose,
                                                       candidate_primary_deg,
                                                       candidate_secondary_deg,
                                                       &candidate_cost_deg))
                    {
                        return false;
                    }

                    if (candidate_cost_deg + 1.0e-6f < best_cost_deg)
                    {
                        best_primary_deg = candidate_primary_deg;
                        best_secondary_deg = candidate_secondary_deg;
                        best_cost_deg = candidate_cost_deg;
                        improved = true;
                    }
                }
            }
        }
    }

    p_state->last_primary_deg = best_primary_deg;
    p_state->last_secondary_deg = best_secondary_deg;
    p_state->initialized = true;

    *p_best_primary_deg = best_primary_deg;
    *p_best_secondary_deg = best_secondary_deg;
    *p_best_cost_deg = best_cost_deg;
    return true;
}

static bool imu_try_build_servo_pose_fk_opt(imu_app_context_t * p_ctx,
                                            imu_motion_components_t const * p_motion,
                                            int32_t * p_hY_deg,
                                            int32_t * p_hZ_deg,
                                            int32_t * p_eZ_deg,
                                            int32_t * p_wX_deg)
{
    float hY_target_deg = 90.0f;
    float hZ_target_deg = 90.0f;
    float eZ_target_deg = 0.0f;
    float wX_target_deg = 90.0f;
    float upper_cost_deg = 0.0f;
    float lower_cost_deg = 0.0f;

    (void) upper_cost_deg;
    (void) lower_cost_deg;

    if ((NULL == p_ctx) || (NULL == p_motion) ||
        (NULL == p_hY_deg) || (NULL == p_hZ_deg) || (NULL == p_eZ_deg) || (NULL == p_wX_deg))
    {
        return false;
    }

    if (!imu_optimize_joint_pair(&p_ctx->calibration.hY_map,
                                 &p_ctx->calibration.hZ_map,
                                 &p_motion->upper_bone,
                                 &p_ctx->calibration.upper_fk_state,
                                 90.0f,
                                 90.0f,
                                 &hY_target_deg,
                                 &hZ_target_deg,
                                 &upper_cost_deg) ||
        !imu_optimize_joint_pair(&p_ctx->calibration.eZ_map,
                                 &p_ctx->calibration.wX_map,
                                 &p_motion->relative_bone,
                                 &p_ctx->calibration.lower_fk_state,
                                 0.0f,
                                 90.0f,
                                 &eZ_target_deg,
                                 &wX_target_deg,
                                 &lower_cost_deg))
    {
        return false;
    }

    if (!imu_apply_servo_target_deg(&p_ctx->calibration.hY_map, hY_target_deg, p_hY_deg) ||
        !imu_apply_servo_target_deg(&p_ctx->calibration.hZ_map, hZ_target_deg, p_hZ_deg) ||
        !imu_apply_servo_target_deg(&p_ctx->calibration.eZ_map, eZ_target_deg, p_eZ_deg) ||
        !imu_apply_servo_target_deg(&p_ctx->calibration.wX_map, wX_target_deg, p_wX_deg))
    {
        return false;
    }

    return true;
}
static imu_cal_result_t imu_learn_axis_map(imu_axis_map_t * p_map,
                                           Quaternion_t const * p_pose,
                                           imu_signal_source_t source,
                                           imu_angle_measure_t measure,
                                           int16_t center_deg,
                                           float target_delta_deg)
{
    /*
     * 学习单个自由度的映射参数。
     * 这里会从当前姿态样本中提取：
     * - 主运动轴；
     * - 当前动作幅度；
     * - 原始动作到目标舵机角的缩放关系。
     */
    /* 把当前标定姿态提取成轴角表达，作为某一个舵机通道的原始学习样本。 */
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

static float imu_measure_axis_separation_deg(icm42688Float3_t const * p_axis1,
                                             icm42688Float3_t const * p_axis2)
{
    float dot_value;

    if ((NULL == p_axis1) || (NULL == p_axis2))
    {
        return 0.0f;
    }

    dot_value = imu_clampf(imu_vector_dot(p_axis1, p_axis2), -1.0f, 1.0f);
    return acosf(dot_value) * IMU_RAD_TO_DEG;
}

static bool imu_quality_metrics_pass(imu_cal_quality_metrics_t const * p_metrics,
                                     float primary_target_1_deg,
                                     float primary_target_2_deg)
{
    if (NULL == p_metrics)
    {
        return false;
    }

    if (p_metrics->basis_det < IMU_CAL_MIN_BASIS_DET)
    {
        return false;
    }

    if (p_metrics->axis_separation_deg < IMU_CAL_MIN_AXIS_SEPARATION_DEG)
    {
        return false;
    }

    if (fabsf(p_metrics->primary_response_1_deg - primary_target_1_deg) > IMU_CAL_PRIMARY_RESPONSE_TOLERANCE_DEG)
    {
        return false;
    }

    if (fabsf(p_metrics->primary_response_2_deg - primary_target_2_deg) > IMU_CAL_PRIMARY_RESPONSE_TOLERANCE_DEG)
    {
        return false;
    }

    if ((fabsf(p_metrics->cross_leak_1_deg) > IMU_CAL_MAX_CROSS_LEAK_DEG) ||
        (fabsf(p_metrics->cross_leak_2_deg) > IMU_CAL_MAX_CROSS_LEAK_DEG))
    {
        return false;
    }

    return true;
}

static bool imu_measure_swing_deg(Quaternion_t const * p_pose,
                                  icm42688Float3_t const * p_reference,
                                  icm42688Float3_t const * p_axis,
                                  float * p_angle_deg)
{
    /*
     * swing 表示绕目标轴之外的摆动。
     * 算法通过把向量投影到垂直于该轴的平面，再在平面内测夹角来估计它。
     */
    /* 通过把参考向量投影到垂直于目标轴的平面上，测量该自由度对应的摆动角。 */
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
    /*
     * twist 表示绕目标轴本身的扭转。
     * 它不是普通摆角，所以这里需要从四元数中单独提取沿主轴的旋转成分。
     */
    /* 通过把四元数虚部投影到学习到的主轴上，尽量把绕该轴的扭转量单独提取出来。 */
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
    /*
     * 把当前旋转向量近似分解成两个标定基向量的线性组合。
     * 分解系数就代表当前姿态里各自由度成分各占多少。
     */
    /* 求当前旋转向量在两个标定基向量上的分解系数，用来估计两个舵机通道各自的贡献。 */
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

static bool imu_measure_axis_map_raw_deg(imu_axis_map_t const * p_map,
                                         imu_motion_components_t const * p_motion,
                                         float * p_raw_deg)
{
    Quaternion_t const * p_pose = NULL;

    if ((NULL == p_map) || (NULL == p_motion) || (NULL == p_raw_deg) || !p_map->valid)
    {
        return false;
    }

    switch (p_map->source)
    {
        case IMU_SIGNAL_SOURCE_UPPER:
            p_pose = &p_motion->upper_bone;
            break;

        case IMU_SIGNAL_SOURCE_RELATIVE:
            p_pose = &p_motion->relative_bone;
            break;

        default:
            return false;
    }

    switch (p_map->measure)
    {
        case IMU_ANGLE_MEASURE_SWING:
            if (!p_map->has_reference)
            {
                return false;
            }

            return imu_measure_swing_deg(p_pose, &p_map->reference, &p_map->axis, p_raw_deg);

        case IMU_ANGLE_MEASURE_TWIST:
            return imu_measure_twist_deg(p_pose, &p_map->axis, p_raw_deg);

        default:
            return false;
    }
}

static bool imu_calc_pose_basis_residual_deg(Quaternion_t const * p_pose,
                                             Quaternion_t const * p_basis1_pose,
                                             Quaternion_t const * p_basis2_pose,
                                             float * p_residual_deg)
{
    icm42688Float3_t current_vec = {0.0f, 0.0f, 0.0f};
    icm42688Float3_t basis1_vec = {0.0f, 0.0f, 0.0f};
    icm42688Float3_t basis2_vec = {0.0f, 0.0f, 0.0f};
    icm42688Float3_t residual_vec = {0.0f, 0.0f, 0.0f};
    float            coeff1 = 0.0f;
    float            coeff2 = 0.0f;

    if ((NULL == p_pose) || (NULL == p_basis1_pose) || (NULL == p_basis2_pose) || (NULL == p_residual_deg))
    {
        return false;
    }

    if (!imu_quaternion_extract_rotation_vector_deg(p_pose, &current_vec) ||
        !imu_quaternion_extract_rotation_vector_deg(p_basis1_pose, &basis1_vec) ||
        !imu_quaternion_extract_rotation_vector_deg(p_basis2_pose, &basis2_vec))
    {
        return false;
    }

    if (!imu_solve_basis_coefficients(&basis1_vec, &basis2_vec, &current_vec, &coeff1, &coeff2))
    {
        return false;
    }

    residual_vec.x = current_vec.x - ((basis1_vec.x * coeff1) + (basis2_vec.x * coeff2));
    residual_vec.y = current_vec.y - ((basis1_vec.y * coeff1) + (basis2_vec.y * coeff2));
    residual_vec.z = current_vec.z - ((basis1_vec.z * coeff1) + (basis2_vec.z * coeff2));
    *p_residual_deg = imu_vector_norm(&residual_vec);

    return true;
}

static bool imu_get_stable_axis_output(imu_axis_map_t const * p_map, int32_t * p_output_deg)
{
    if ((NULL == p_map) || (NULL == p_output_deg) || !p_map->valid)
    {
        return false;
    }

    *p_output_deg = p_map->has_last_output ? (int32_t) p_map->last_output_deg : (int32_t) p_map->center_deg;
    return true;
}

static bool imu_try_build_upper_servo_pose_direct(imu_app_context_t * p_ctx,
                                                  imu_motion_components_t const * p_motion,
                                                  int32_t * p_hY_deg,
                                                  int32_t * p_hZ_deg)
{
    float upper_hy_raw_deg = 0.0f;
    float upper_hz_raw_deg = 0.0f;
    float upper_residual_deg = 0.0f;
    bool  upper_hold;

    if ((NULL == p_ctx) || (NULL == p_motion) || (NULL == p_hY_deg) || (NULL == p_hZ_deg))
    {
        return false;
    }

    upper_hold = !imu_measure_axis_map_raw_deg(&p_ctx->calibration.hY_map, p_motion, &upper_hy_raw_deg) ||
                 !imu_measure_axis_map_raw_deg(&p_ctx->calibration.hZ_map, p_motion, &upper_hz_raw_deg) ||
                 !imu_calc_pose_basis_residual_deg(&p_motion->upper_bone,
                                                   &p_ctx->calibration.upper_hy_pose,
                                                   &p_ctx->calibration.upper_hz_pose,
                                                   &upper_residual_deg) ||
                 (upper_residual_deg > IMU_RUNTIME_POSE_RESIDUAL_HOLD_DEG);

    if (upper_hold)
    {
        if (!imu_get_stable_axis_output(&p_ctx->calibration.hY_map, p_hY_deg) ||
            !imu_get_stable_axis_output(&p_ctx->calibration.hZ_map, p_hZ_deg))
        {
            return false;
        }
    }
    else if (!imu_apply_axis_map(&p_ctx->calibration.hY_map,
                                 upper_hy_raw_deg * p_ctx->calibration.hY_map.gain,
                                 p_hY_deg) ||
             !imu_apply_axis_map(&p_ctx->calibration.hZ_map,
                                 upper_hz_raw_deg * p_ctx->calibration.hZ_map.gain,
                                 p_hZ_deg))
    {
        return false;
    }

    return true;
}

static bool imu_try_build_lower_servo_pose_hybrid(imu_app_context_t * p_ctx,
                                                  imu_motion_components_t const * p_motion,
                                                  int32_t * p_eZ_deg,
                                                  int32_t * p_wX_deg)
{
    float relative_ez_raw_deg = 0.0f;
    float relative_wx_raw_deg = 0.0f;
    float lower_residual_deg = 0.0f;
    float fk_eZ_target_deg = 0.0f;
    float fk_wX_target_deg = 90.0f;
    float fk_lower_cost_deg = 0.0f;
    bool  lower_ez_direct_valid;
    bool  lower_wx_direct_valid;
    bool  lower_residual_too_large;
    bool  lower_fk_valid;

    if ((NULL == p_ctx) || (NULL == p_motion) || (NULL == p_eZ_deg) || (NULL == p_wX_deg))
    {
        return false;
    }

    lower_ez_direct_valid = imu_measure_axis_map_raw_deg(&p_ctx->calibration.eZ_map, p_motion, &relative_ez_raw_deg);
    lower_wx_direct_valid = imu_measure_axis_map_raw_deg(&p_ctx->calibration.wX_map, p_motion, &relative_wx_raw_deg);

    lower_residual_too_large =
        !imu_calc_pose_basis_residual_deg(&p_motion->relative_bone,
                                          &p_ctx->calibration.relative_ez_pose,
                                          &p_ctx->calibration.relative_wx_pose,
                                          &lower_residual_deg) ||
        (lower_residual_deg > IMU_RUNTIME_LOWER_RESIDUAL_HOLD_DEG);

    lower_fk_valid = imu_optimize_joint_pair(&p_ctx->calibration.eZ_map,
                                             &p_ctx->calibration.wX_map,
                                             &p_motion->relative_bone,
                                             &p_ctx->calibration.lower_fk_state,
                                             0.0f,
                                             90.0f,
                                             &fk_eZ_target_deg,
                                             &fk_wX_target_deg,
                                             &fk_lower_cost_deg);

    if (lower_ez_direct_valid && !lower_residual_too_large)
    {
        if (!imu_apply_axis_map(&p_ctx->calibration.eZ_map,
                                relative_ez_raw_deg * p_ctx->calibration.eZ_map.gain,
                                p_eZ_deg))
        {
            return false;
        }
    }
    else if (lower_fk_valid)
    {
        if (!imu_apply_servo_target_deg(&p_ctx->calibration.eZ_map, fk_eZ_target_deg, p_eZ_deg))
        {
            return false;
        }
    }
    else if (!imu_get_stable_axis_output(&p_ctx->calibration.eZ_map, p_eZ_deg))
    {
        return false;
    }

    if (lower_fk_valid)
    {
        if (!imu_apply_servo_target_deg(&p_ctx->calibration.wX_map, fk_wX_target_deg, p_wX_deg))
        {
            return false;
        }
    }
    else if (lower_wx_direct_valid && !lower_residual_too_large)
    {
        if (!imu_apply_axis_map(&p_ctx->calibration.wX_map,
                                relative_wx_raw_deg * p_ctx->calibration.wX_map.gain,
                                p_wX_deg))
        {
            return false;
        }
    }
    else if (!imu_get_stable_axis_output(&p_ctx->calibration.wX_map, p_wX_deg))
    {
        return false;
    }

    (void) fk_lower_cost_deg;
    return true;
}

static bool imu_finalize_upper_axis_maps(imu_app_context_t * p_ctx)
{
    float                     best_valid_score = 1.0e9f;
    float                     best_any_score = 1.0e9f;
    bool                      found_valid = false;
    bool                      found_any = false;
    imu_axis_map_t            hy_map = p_ctx->calibration.hY_map;
    imu_axis_map_t            hz_map = p_ctx->calibration.hZ_map;
    imu_axis_map_t            best_hy_map = hy_map;
    imu_axis_map_t            best_hz_map = hz_map;
    imu_axis_map_t            best_any_hy_map = hy_map;
    imu_axis_map_t            best_any_hz_map = hz_map;
    imu_cal_quality_metrics_t best_metrics = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    imu_cal_quality_metrics_t best_any_metrics = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

    for (int hy_sign = -1; hy_sign <= 1; hy_sign += 2)
    {
        for (int hz_sign = -1; hz_sign <= 1; hz_sign += 2)
        {
            imu_axis_map_t            candidate_hy_map = hy_map;
            imu_axis_map_t            candidate_hz_map = hz_map;
            imu_cal_quality_metrics_t metrics = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
            icm42688Float3_t          axis_hy = {hy_map.axis.x * (float) hy_sign,
                                                 hy_map.axis.y * (float) hy_sign,
                                                 hy_map.axis.z * (float) hy_sign};
            icm42688Float3_t          axis_hz = {hz_map.axis.x * (float) hz_sign,
                                                 hz_map.axis.y * (float) hz_sign,
                                                 hz_map.axis.z * (float) hz_sign};
            icm42688Float3_t          reference = imu_vector_cross(&axis_hy, &axis_hz);
            float                     hy_raw_deg = 0.0f;
            float                     hz_raw_deg = 0.0f;
            float                     hz_on_hy_deg = 0.0f;
            float                     hy_on_hz_deg = 0.0f;
            float                     score;

            if (!imu_vector_normalize_unit(&reference))
            {
                continue;
            }

            if (!imu_measure_swing_deg(&p_ctx->calibration.upper_hy_pose, &reference, &axis_hy, &hy_raw_deg) ||
                !imu_measure_swing_deg(&p_ctx->calibration.upper_hz_pose, &reference, &axis_hz, &hz_raw_deg) ||
                !imu_measure_swing_deg(&p_ctx->calibration.upper_hy_pose, &reference, &axis_hz, &hz_on_hy_deg) ||
                !imu_measure_swing_deg(&p_ctx->calibration.upper_hz_pose, &reference, &axis_hy, &hy_on_hz_deg))
            {
                continue;
            }

            if ((fabsf(hy_raw_deg) < IMU_AXIS_MIN_RESPONSE_DEG) ||
                (fabsf(hz_raw_deg) < IMU_AXIS_MIN_RESPONSE_DEG))
            {
                continue;
            }

            metrics.basis_det = (imu_vector_dot(&axis_hy, &axis_hy) * imu_vector_dot(&axis_hz, &axis_hz)) -
                                (imu_vector_dot(&axis_hy, &axis_hz) * imu_vector_dot(&axis_hy, &axis_hz));
            metrics.axis_separation_deg = imu_measure_axis_separation_deg(&axis_hy, &axis_hz);
            metrics.primary_response_1_deg = hy_raw_deg;
            metrics.primary_response_2_deg = hz_raw_deg;
            metrics.cross_leak_1_deg = hz_on_hy_deg;
            metrics.cross_leak_2_deg = hy_on_hz_deg;

            score = fabsf(IMU_CAL_TARGET_DELTA_DEG - hy_raw_deg) +
                    fabsf((-IMU_CAL_TARGET_DELTA_DEG) - hz_raw_deg);

            candidate_hy_map.axis = axis_hy;
            candidate_hy_map.reference = reference;
            candidate_hy_map.gain = IMU_CAL_TARGET_DELTA_DEG / hy_raw_deg;
            candidate_hy_map.has_reference = true;
            candidate_hy_map.has_filtered_raw = false;
            candidate_hy_map.has_last_raw = false;
            candidate_hy_map.has_last_output = false;
            candidate_hy_map.last_raw_deg = 0.0f;
            candidate_hy_map.filtered_raw_deg = 0.0f;
            candidate_hy_map.last_output_deg = candidate_hy_map.center_deg;

            candidate_hz_map.axis = axis_hz;
            candidate_hz_map.reference = reference;
            candidate_hz_map.gain = (-IMU_CAL_TARGET_DELTA_DEG) / hz_raw_deg;
            candidate_hz_map.has_reference = true;
            candidate_hz_map.has_filtered_raw = false;
            candidate_hz_map.has_last_raw = false;
            candidate_hz_map.has_last_output = false;
            candidate_hz_map.last_raw_deg = 0.0f;
            candidate_hz_map.filtered_raw_deg = 0.0f;
            candidate_hz_map.last_output_deg = candidate_hz_map.center_deg;

            if ((!found_any) || (score < best_any_score))
            {
                found_any = true;
                best_any_score = score;
                best_any_hy_map = candidate_hy_map;
                best_any_hz_map = candidate_hz_map;
                best_any_metrics = metrics;
            }

            if (imu_quality_metrics_pass(&metrics, IMU_CAL_TARGET_DELTA_DEG, -IMU_CAL_TARGET_DELTA_DEG) &&
                ((!found_valid) || (score < best_valid_score)))
            {
                found_valid = true;
                best_valid_score = score;
                best_hy_map = candidate_hy_map;
                best_hz_map = candidate_hz_map;
                best_metrics = metrics;
            }
        }
    }

    p_ctx->calibration.upper_quality = found_any ? best_any_metrics : best_metrics;
    if (!found_valid)
    {
        if (!found_any)
        {
            return false;
        }

        /*
         * 人手标定时动作经常不够标准。若严格质量门限未通过，
         * 但仍能找到“主响应最接近目标动作”的候选解，则退化采用该解，
         * 让标定更容易通过，运行时再依靠输出滤波和 FK 优化抑制误差。
         */
        p_ctx->calibration.hY_map = best_any_hy_map;
        p_ctx->calibration.hZ_map = best_any_hz_map;
        return true;
    }

    p_ctx->calibration.hY_map = best_hy_map;
    p_ctx->calibration.hZ_map = best_hz_map;
    p_ctx->calibration.upper_quality = best_metrics;
    return true;
}

static bool imu_finalize_lower_axis_maps(imu_app_context_t * p_ctx)
{
    float                     best_valid_score = 1.0e9f;
    float                     best_any_score = 1.0e9f;
    bool                      found_valid = false;
    bool                      found_any = false;
    imu_axis_map_t            ez_map = p_ctx->calibration.eZ_map;
    imu_axis_map_t            wx_map = p_ctx->calibration.wX_map;
    imu_axis_map_t            best_ez_map = ez_map;
    imu_axis_map_t            best_wx_map = wx_map;
    imu_axis_map_t            best_any_ez_map = ez_map;
    imu_axis_map_t            best_any_wx_map = wx_map;
    imu_cal_quality_metrics_t best_metrics = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    imu_cal_quality_metrics_t best_any_metrics = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

    for (int ez_sign = -1; ez_sign <= 1; ez_sign += 2)
    {
        for (int wx_sign = -1; wx_sign <= 1; wx_sign += 2)
        {
            imu_axis_map_t            candidate_ez_map = ez_map;
            imu_axis_map_t            candidate_wx_map = wx_map;
            imu_cal_quality_metrics_t metrics = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
            icm42688Float3_t          axis_ez = {ez_map.axis.x * (float) ez_sign,
                                                 ez_map.axis.y * (float) ez_sign,
                                                 ez_map.axis.z * (float) ez_sign};
            icm42688Float3_t          axis_wx = {wx_map.axis.x * (float) wx_sign,
                                                 wx_map.axis.y * (float) wx_sign,
                                                 wx_map.axis.z * (float) wx_sign};
            float                     ez_raw_deg = 0.0f;
            float                     wx_raw_deg = 0.0f;
            float                     wx_on_ez_deg = 0.0f;
            float                     ez_on_wx_deg = 0.0f;
            float                     score;

            if (!imu_vector_normalize_unit(&axis_wx))
            {
                continue;
            }

            if (!imu_measure_swing_deg(&p_ctx->calibration.relative_ez_pose, &axis_wx, &axis_ez, &ez_raw_deg) ||
                !imu_measure_twist_deg(&p_ctx->calibration.relative_wx_pose, &axis_wx, &wx_raw_deg) ||
                !imu_measure_twist_deg(&p_ctx->calibration.relative_ez_pose, &axis_wx, &wx_on_ez_deg) ||
                !imu_measure_swing_deg(&p_ctx->calibration.relative_wx_pose, &axis_wx, &axis_ez, &ez_on_wx_deg))
            {
                continue;
            }

            if ((fabsf(ez_raw_deg) < IMU_AXIS_MIN_RESPONSE_DEG) ||
                (fabsf(wx_raw_deg) < IMU_AXIS_MIN_RESPONSE_DEG))
            {
                continue;
            }

            metrics.basis_det = (imu_vector_dot(&axis_ez, &axis_ez) * imu_vector_dot(&axis_wx, &axis_wx)) -
                                (imu_vector_dot(&axis_ez, &axis_wx) * imu_vector_dot(&axis_ez, &axis_wx));
            metrics.axis_separation_deg = imu_measure_axis_separation_deg(&axis_ez, &axis_wx);
            metrics.primary_response_1_deg = ez_raw_deg;
            metrics.primary_response_2_deg = wx_raw_deg;
            metrics.cross_leak_1_deg = wx_on_ez_deg;
            metrics.cross_leak_2_deg = ez_on_wx_deg;

            score = fabsf(IMU_CAL_TARGET_DELTA_DEG - ez_raw_deg) +
                    fabsf(IMU_CAL_TARGET_DELTA_DEG - wx_raw_deg);

            candidate_ez_map.axis = axis_ez;
            candidate_ez_map.reference = axis_wx;
            candidate_ez_map.gain = IMU_CAL_TARGET_DELTA_DEG / ez_raw_deg;
            candidate_ez_map.has_reference = true;
            candidate_ez_map.has_filtered_raw = false;
            candidate_ez_map.has_last_raw = false;
            candidate_ez_map.has_last_output = false;
            candidate_ez_map.last_raw_deg = 0.0f;
            candidate_ez_map.filtered_raw_deg = 0.0f;
            candidate_ez_map.last_output_deg = candidate_ez_map.center_deg;

            candidate_wx_map.axis = axis_wx;
            candidate_wx_map.reference = axis_wx;
            candidate_wx_map.gain = IMU_CAL_TARGET_DELTA_DEG / wx_raw_deg;
            candidate_wx_map.has_reference = true;
            candidate_wx_map.has_filtered_raw = false;
            candidate_wx_map.has_last_raw = false;
            candidate_wx_map.has_last_output = false;
            candidate_wx_map.last_raw_deg = 0.0f;
            candidate_wx_map.filtered_raw_deg = 0.0f;
            candidate_wx_map.last_output_deg = candidate_wx_map.center_deg;

            if ((!found_any) || (score < best_any_score))
            {
                found_any = true;
                best_any_score = score;
                best_any_ez_map = candidate_ez_map;
                best_any_wx_map = candidate_wx_map;
                best_any_metrics = metrics;
            }

            if (imu_quality_metrics_pass(&metrics, IMU_CAL_TARGET_DELTA_DEG, IMU_CAL_TARGET_DELTA_DEG) &&
                ((!found_valid) || (score < best_valid_score)))
            {
                found_valid = true;
                best_valid_score = score;
                best_ez_map = candidate_ez_map;
                best_wx_map = candidate_wx_map;
                best_metrics = metrics;
            }
        }
    }

    p_ctx->calibration.lower_quality = found_any ? best_any_metrics : best_metrics;
    if (!found_valid)
    {
        if (!found_any)
        {
            return false;
        }

        p_ctx->calibration.eZ_map = best_any_ez_map;
        p_ctx->calibration.wX_map = best_any_wx_map;
        return true;
    }

    p_ctx->calibration.eZ_map = best_ez_map;
    p_ctx->calibration.wX_map = best_wx_map;
    p_ctx->calibration.lower_quality = best_metrics;
    return true;
}

static imu_cal_result_t imu_record_current_step(imu_app_context_t * p_ctx, uint32_t now_us)
{
    /*
     * 标定流程调度器。
     * 根据当前所处步骤，把这一帧姿态解释成 T 姿态偏置或某个自由度的学习样本。
     */
    /* 按照当前所处的标定步骤，解释这一刻的姿态，并把结果写入对应映射结构。 */
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
        imu_reset_fk_warm_start(p_ctx);
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
            if (!imu_finalize_lower_axis_maps(p_ctx))
            {
                return IMU_CAL_RESULT_AMBIG;
            }

            imu_refine_zero_drift_after_final_step(p_ctx);
            return IMU_CAL_RESULT_OK;
        }

        default:
            return IMU_CAL_RESULT_AMBIG;
    }
}

static void imu_refine_zero_drift_after_final_step(imu_app_context_t * p_ctx)
{
    if (NULL == p_ctx)
    {
        return;
    }

    (void) imu_refine_gyro_bias(&p_ctx->upper_imu,
                                bsp_IcmGetScaledData,
                                IMU_FINAL_DRIFT_SAMPLES,
                                IMU_FINAL_DRIFT_MAX_ATTEMPTS,
                                IMU_FINAL_DRIFT_ACC_MIN_G,
                                IMU_FINAL_DRIFT_ACC_MAX_G,
                                IMU_FINAL_DRIFT_GYRO_MAX_RAD_S);
    (void) imu_refine_gyro_bias(&p_ctx->lower_imu,
                                bsp_IcmSciGetScaledData,
                                IMU_FINAL_DRIFT_SAMPLES,
                                IMU_FINAL_DRIFT_MAX_ATTEMPTS,
                                IMU_FINAL_DRIFT_ACC_MIN_G,
                                IMU_FINAL_DRIFT_ACC_MAX_G,
                                IMU_FINAL_DRIFT_GYRO_MAX_RAD_S);
}

static bool imu_apply_axis_output_common(imu_axis_map_t * p_map,
                                         float raw_deg,
                                         bool wrap_delta,
                                         int32_t * p_output_deg)
{
    /*
     * 输出整形层：
     * 限速、滤波、死区都在这里执行，用来减少抖动和突变。
     */
    /* 在输出舵机角前做限速、滤波和死区处理，减少抖动和跳变。 */
    float   delta_deg;
    int32_t output_deg;

    if ((NULL == p_map) || (NULL == p_output_deg) || !p_map->valid)
    {
        return false;
    }

    if (p_map->has_last_raw)
    {
        delta_deg = raw_deg - p_map->last_raw_deg;
        if (wrap_delta)
        {
            while (delta_deg > 180.0f)
            {
                delta_deg -= 360.0f;
            }

            while (delta_deg < -180.0f)
            {
                delta_deg += 360.0f;
            }
        }
        delta_deg = imu_clampf(delta_deg, -IMU_RUNTIME_MAX_STEP_DEG, IMU_RUNTIME_MAX_STEP_DEG);
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

static bool imu_apply_axis_map(imu_axis_map_t * p_map, float raw_deg, int32_t * p_output_deg)
{
    /*
     * 杈撳嚭鏁村舰灞傦細
     * 闄愰€熴€佹护娉€佹鍖洪兘鍦ㄨ繖閲屾墽琛岋紝鐢ㄦ潵鍑忓皯鎶栧姩鍜岀獊鍙樸€?     */
    /* 鍦ㄨ緭鍑鸿埖鏈鸿鍓嶅仛闄愰€熴€佹护娉㈠拰姝诲尯澶勭悊锛屽噺灏戞姈鍔ㄥ拰璺冲彉銆?*/
    bool wrap_delta;

    if (NULL == p_map)
    {
        return false;
    }

    wrap_delta = (IMU_ANGLE_MEASURE_TWIST == p_map->measure);

    return imu_apply_axis_output_common(p_map, raw_deg, wrap_delta, p_output_deg);
}

static bool imu_apply_servo_target_deg(imu_axis_map_t * p_map, float target_deg, int32_t * p_output_deg)
{
    if ((NULL == p_map) || (NULL == p_output_deg))
    {
        return false;
    }

    return imu_apply_axis_output_common(p_map, target_deg - (float) p_map->center_deg, false, p_output_deg);
}
static uint8_t imu_get_grip_percent(void)
{
#if 0
    /*
     * 正式融合模式下，手掌张合度由 EMG 算法接管。
     * 这里故意只暴露最终 grip 百分比，不把 EMG 内部状态泄漏到 IMU 标定模块，
     * 让 IMU 侧继续只关心“我现在应该发多少 grip”。
     */
    /* Mainline IMU mapping does not drive the grip channel. */
    return 0U;
#endif
    return arm_get_emg_grip_percent();
}
