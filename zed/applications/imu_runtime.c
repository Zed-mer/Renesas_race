#include "imu_runtime.h"
#include "imu_math.h"
#include <math.h>

#define MAHONY_KP  2.5f
#define MAHONY_KI  0.08f

/*
 * 运行时姿态解算模块：
 * 负责时间戳、零偏估计和 Mahony 姿态融合。
 */

static float imu_update_filtered_temperature(imu_runtime_t * p_imu, float temperature_c);
static bool  imu_can_learn_temperature_compensation(icm42688Float3_t const * p_acc_g,
                                                    icm42688Float3_t const * p_corrected_gyro_rad_s);

void imu_runtime_reset(imu_runtime_t * p_imu)
{
    /* 重置单个 IMU 的运行时状态，保证开机和重新标定都从干净状态开始。 */
    if (NULL == p_imu)
    {
        return;
    }

    imu_quaternion_identity(&p_imu->quat);
    p_imu->gyro_bias.x = 0.0f;
    p_imu->gyro_bias.y = 0.0f;
    p_imu->gyro_bias.z = 0.0f;
    p_imu->gyro_temp_slope.x = 0.0f;
    p_imu->gyro_temp_slope.y = 0.0f;
    p_imu->gyro_temp_slope.z = 0.0f;
    p_imu->mahony_integral.x = 0.0f;
    p_imu->mahony_integral.y = 0.0f;
    p_imu->mahony_integral.z = 0.0f;
    p_imu->bias_temperature_c = 0.0f;
    p_imu->current_temperature_c = 0.0f;
    p_imu->filtered_temperature_c = 0.0f;
    p_imu->last_sample_time_us = 0U;
    p_imu->data_ready_time_us = 0U;
    p_imu->pending_ready_count = 0U;
    p_imu->has_temperature_reference = false;
    p_imu->has_filtered_temperature = false;
}

void imu_timebase_init(imu_timebase_t * p_timebase)
{
    /* 使用 CPU 周期计数器构建轻量级微秒时基，供传感器时间戳和 dt 计算使用。 */
    if (NULL == p_timebase)
    {
        return;
    }

#if BSP_FEATURE_DWT_CYCCNT
    DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
#endif

    p_timebase->last_cycle_count = 0U;
    p_timebase->cycle_accumulator = 0U;
    p_timebase->cycles_per_us = R_FSP_SystemClockHzGet(FSP_PRIV_CLOCK_ICLK) / 1000000U;
    if (0U == p_timebase->cycles_per_us)
    {
        p_timebase->cycles_per_us = 1U;
    }
}

uint32_t imu_time_now_us(imu_timebase_t * p_timebase)
{
    if (NULL == p_timebase)
    {
        return 0U;
    }

#if BSP_FEATURE_DWT_CYCCNT
    {
        uint32_t current_cycle_count = DWT->CYCCNT;
        uint32_t cycle_delta = current_cycle_count - p_timebase->last_cycle_count;

        p_timebase->last_cycle_count = current_cycle_count;
        p_timebase->cycle_accumulator += cycle_delta;

        return (uint32_t) (p_timebase->cycle_accumulator / p_timebase->cycles_per_us);
    }
#else
    return 0U;
#endif
}

void imu_mark_data_ready(imu_runtime_t * p_imu, imu_timebase_t * p_timebase)
{
    if (NULL == p_imu)
    {
        return;
    }

    p_imu->data_ready_time_us = imu_time_now_us(p_timebase);
    if (p_imu->pending_ready_count < UINT16_MAX)
    {
        p_imu->pending_ready_count++;
    }
}

float imu_calc_dt_sec(imu_runtime_t * p_imu, uint32_t sample_time_us)
{
    /* 把 dt 限制在合理范围，避免中断丢失或时间异常时把姿态滤波器冲坏。 */
    float dt_sec = IMU_SAMPLE_DT_DEFAULT_SEC;

    if (0U != p_imu->last_sample_time_us)
    {
        uint32_t delta_us = sample_time_us - p_imu->last_sample_time_us;

        dt_sec = (float) delta_us / 1000000.0f;
        if (dt_sec < IMU_SAMPLE_DT_MIN_SEC)
        {
            dt_sec = IMU_SAMPLE_DT_MIN_SEC;
        }
        else if (dt_sec > IMU_SAMPLE_DT_MAX_SEC)
        {
            dt_sec = IMU_SAMPLE_DT_MAX_SEC;
        }
    }

    p_imu->last_sample_time_us = sample_time_us;

    return dt_sec;
}

void imu_mahony_update(imu_runtime_t * p_imu,
                       icm42688Float3_t const * p_acc_g,
                       icm42688Float3_t const * p_gyro_rad_s,
                       float dt_sec)
{
    /*
     * Mahony 滤波：
     * 陀螺积分负责快速响应，加速度重力方向负责慢速纠偏。
     */
    /* Mahony 姿态更新：陀螺积分负责快速响应，加速度计重力方向负责慢速纠偏。 */
    float acc_norm;
    float gx = p_gyro_rad_s->x;
    float gy = p_gyro_rad_s->y;
    float gz = p_gyro_rad_s->z;

    acc_norm = imu_vector_norm(p_acc_g);
    if ((acc_norm >= IMU_CORRECTION_ACC_MIN_G) && (acc_norm <= IMU_CORRECTION_ACC_MAX_G))
    {
        float ax = p_acc_g->x / acc_norm;
        float ay = p_acc_g->y / acc_norm;
        float az = p_acc_g->z / acc_norm;
        float vx = 2.0f * ((p_imu->quat.q1 * p_imu->quat.q3) - (p_imu->quat.q0 * p_imu->quat.q2));
        float vy = 2.0f * ((p_imu->quat.q0 * p_imu->quat.q1) + (p_imu->quat.q2 * p_imu->quat.q3));
        float vz = (p_imu->quat.q0 * p_imu->quat.q0) - (p_imu->quat.q1 * p_imu->quat.q1) -
                   (p_imu->quat.q2 * p_imu->quat.q2) + (p_imu->quat.q3 * p_imu->quat.q3);
        float ex = (ay * vz) - (az * vy);
        float ey = (az * vx) - (ax * vz);
        float ez = (ax * vy) - (ay * vx);

        p_imu->mahony_integral.x += MAHONY_KI * ex * dt_sec;
        p_imu->mahony_integral.y += MAHONY_KI * ey * dt_sec;
        p_imu->mahony_integral.z += MAHONY_KI * ez * dt_sec;

        gx += (MAHONY_KP * ex) + p_imu->mahony_integral.x;
        gy += (MAHONY_KP * ey) + p_imu->mahony_integral.y;
        gz += (MAHONY_KP * ez) + p_imu->mahony_integral.z;
    }

    {
        float q0 = p_imu->quat.q0;
        float q1 = p_imu->quat.q1;
        float q2 = p_imu->quat.q2;
        float q3 = p_imu->quat.q3;
        float half_dt = 0.5f * dt_sec;

        p_imu->quat.q0 += (-q1 * gx - q2 * gy - q3 * gz) * half_dt;
        p_imu->quat.q1 += (q0 * gx + q2 * gz - q3 * gy) * half_dt;
        p_imu->quat.q2 += (q0 * gy - q1 * gz + q3 * gx) * half_dt;
        p_imu->quat.q3 += (q0 * gz + q1 * gy - q2 * gx) * half_dt;
    }

    imu_quaternion_normalize(&p_imu->quat);
}

uint32_t imu_collect_gyro_bias(imu_runtime_t * p_imu,
                               imu_read_sample_fn_t read_sample,
                               imu_timebase_t * p_timebase,
                               imu_runtime_wait_hook_t wait_hook,
                               void * p_wait_context)
{
    /*
     * 启动时估计陀螺零偏，避免后续积分姿态发生慢漂。
     */
    /* 在传感器看起来处于静止状态时，对陀螺输出做平均，估计启动零偏。 */
    uint32_t         attempts = 0U;
    uint32_t         valid_samples = 0U;
    icm42688Float3_t gyro_sum = {0.0f, 0.0f, 0.0f};
    float            temperature_sum_c = 0.0f;

    while ((attempts < IMU_CALIBRATION_MAX_ATTEMPTS) && (valid_samples < IMU_CALIBRATION_SAMPLES))
    {
        icm42688Float3_t acc_g = {0.0f, 0.0f, 0.0f};
        icm42688Float3_t gyro_rad_s = {0.0f, 0.0f, 0.0f};
        float            temperature_c = 0.0f;
        float            acc_norm;

        if (NULL != wait_hook)
        {
            wait_hook(p_wait_context, imu_time_now_us(p_timebase));
        }

        imu_read_sample_blocking(p_imu, read_sample, &acc_g, &gyro_rad_s, &temperature_c);
        attempts++;

        acc_norm = imu_vector_norm(&acc_g);
        if ((acc_norm < IMU_STATIC_ACC_MIN_G) || (acc_norm > IMU_STATIC_ACC_MAX_G))
        {
            continue;
        }

        gyro_sum.x += gyro_rad_s.x;
        gyro_sum.y += gyro_rad_s.y;
        gyro_sum.z += gyro_rad_s.z;
        temperature_sum_c += temperature_c;
        valid_samples++;
    }

    if (0U == valid_samples)
    {
        p_imu->gyro_bias.x = 0.0f;
        p_imu->gyro_bias.y = 0.0f;
        p_imu->gyro_bias.z = 0.0f;
        p_imu->bias_temperature_c = 0.0f;
        p_imu->current_temperature_c = 0.0f;
        p_imu->filtered_temperature_c = 0.0f;
        p_imu->has_temperature_reference = false;
        p_imu->has_filtered_temperature = false;
        return 0U;
    }

    p_imu->gyro_bias.x = gyro_sum.x / (float) valid_samples;
    p_imu->gyro_bias.y = gyro_sum.y / (float) valid_samples;
    p_imu->gyro_bias.z = gyro_sum.z / (float) valid_samples;
    p_imu->bias_temperature_c = temperature_sum_c / (float) valid_samples;
    p_imu->current_temperature_c = p_imu->bias_temperature_c;
    p_imu->filtered_temperature_c = p_imu->bias_temperature_c;
    p_imu->has_temperature_reference = true;
    p_imu->has_filtered_temperature = true;

    return valid_samples;
}

void imu_apply_temperature_compensation(imu_runtime_t * p_imu,
                                        icm42688Float3_t const * p_acc_g,
                                        icm42688Float3_t const * p_raw_gyro_rad_s,
                                        float temperature_c,
                                        icm42688Float3_t * p_corrected_gyro_rad_s,
                                        icm42688Float3_t * p_effective_bias_rad_s)
{
    icm42688Float3_t effective_bias = {0.0f, 0.0f, 0.0f};
    float            filtered_temperature_c;
    float            temperature_delta_c;

    if ((NULL == p_imu) || (NULL == p_raw_gyro_rad_s) || (NULL == p_corrected_gyro_rad_s))
    {
        return;
    }

    filtered_temperature_c = imu_update_filtered_temperature(p_imu, temperature_c);
    if (!p_imu->has_temperature_reference)
    {
        p_imu->bias_temperature_c = filtered_temperature_c;
        p_imu->has_temperature_reference = true;
    }

    temperature_delta_c = filtered_temperature_c - p_imu->bias_temperature_c;
    effective_bias.x = p_imu->gyro_bias.x + (p_imu->gyro_temp_slope.x * temperature_delta_c);
    effective_bias.y = p_imu->gyro_bias.y + (p_imu->gyro_temp_slope.y * temperature_delta_c);
    effective_bias.z = p_imu->gyro_bias.z + (p_imu->gyro_temp_slope.z * temperature_delta_c);
    p_corrected_gyro_rad_s->x = p_raw_gyro_rad_s->x - effective_bias.x;
    p_corrected_gyro_rad_s->y = p_raw_gyro_rad_s->y - effective_bias.y;
    p_corrected_gyro_rad_s->z = p_raw_gyro_rad_s->z - effective_bias.z;

    /*
     * 温度补偿采用线性近似：
     * bias(T) = bias_ref + slope * (T - T_ref)
     *
     * 这里不直接在运动中学习，而是只有在“加速度接近 1g 且残余角速度较小”的静止窗口内，
     * 才慢慢修正模型，避免把真实动作误学成温漂。
     */
    if (imu_can_learn_temperature_compensation(p_acc_g, p_corrected_gyro_rad_s))
    {
        if (fabsf(temperature_delta_c) >= IMU_TEMP_COMP_MIN_DELTA_C)
        {
            float slope_gain = IMU_TEMP_COMP_SLOPE_ALPHA / temperature_delta_c;

            p_imu->gyro_temp_slope.x = imu_clampf(p_imu->gyro_temp_slope.x +
                                                  (p_corrected_gyro_rad_s->x * slope_gain),
                                                  -IMU_TEMP_COMP_MAX_SLOPE_RAD_S_PER_C,
                                                  IMU_TEMP_COMP_MAX_SLOPE_RAD_S_PER_C);
            p_imu->gyro_temp_slope.y = imu_clampf(p_imu->gyro_temp_slope.y +
                                                  (p_corrected_gyro_rad_s->y * slope_gain),
                                                  -IMU_TEMP_COMP_MAX_SLOPE_RAD_S_PER_C,
                                                  IMU_TEMP_COMP_MAX_SLOPE_RAD_S_PER_C);
            p_imu->gyro_temp_slope.z = imu_clampf(p_imu->gyro_temp_slope.z +
                                                  (p_corrected_gyro_rad_s->z * slope_gain),
                                                  -IMU_TEMP_COMP_MAX_SLOPE_RAD_S_PER_C,
                                                  IMU_TEMP_COMP_MAX_SLOPE_RAD_S_PER_C);
        }
        else
        {
            /*
             * 温度差还很小时，不强行更新斜率，只对常量 bias 做轻微回正。
             * 这能把“启动均值没采准”与“真正的温漂”分开处理。
             */
            p_imu->gyro_bias.x += IMU_TEMP_COMP_REBIAS_ALPHA * p_corrected_gyro_rad_s->x;
            p_imu->gyro_bias.y += IMU_TEMP_COMP_REBIAS_ALPHA * p_corrected_gyro_rad_s->y;
            p_imu->gyro_bias.z += IMU_TEMP_COMP_REBIAS_ALPHA * p_corrected_gyro_rad_s->z;
            p_imu->bias_temperature_c += IMU_TEMP_COMP_REBIAS_ALPHA *
                                         (filtered_temperature_c - p_imu->bias_temperature_c);
        }

        temperature_delta_c = filtered_temperature_c - p_imu->bias_temperature_c;
        effective_bias.x = p_imu->gyro_bias.x + (p_imu->gyro_temp_slope.x * temperature_delta_c);
        effective_bias.y = p_imu->gyro_bias.y + (p_imu->gyro_temp_slope.y * temperature_delta_c);
        effective_bias.z = p_imu->gyro_bias.z + (p_imu->gyro_temp_slope.z * temperature_delta_c);
        p_corrected_gyro_rad_s->x = p_raw_gyro_rad_s->x - effective_bias.x;
        p_corrected_gyro_rad_s->y = p_raw_gyro_rad_s->y - effective_bias.y;
        p_corrected_gyro_rad_s->z = p_raw_gyro_rad_s->z - effective_bias.z;
    }

    if (NULL != p_effective_bias_rad_s)
    {
        *p_effective_bias_rad_s = effective_bias;
    }
}

void imu_read_sample_blocking(imu_runtime_t * p_imu,
                              imu_read_sample_fn_t read_sample,
                              icm42688Float3_t * p_acc_g,
                              icm42688Float3_t * p_gyro_rad_s,
                              float * p_temp_c)
{
    /* 启动阶段允许阻塞等待下一帧数据就绪，确保零偏采样拿到的是新数据。 */
    FSP_CRITICAL_SECTION_DEFINE;
    uint32_t wait_ms = 0U;

    while ((0U == p_imu->pending_ready_count) && (wait_ms < IMU_IRQ_WAIT_TIMEOUT_MS))
    {
        R_BSP_SoftwareDelay(1U, BSP_DELAY_UNITS_MILLISECONDS);
        wait_ms++;
    }

    FSP_CRITICAL_SECTION_ENTER;
    p_imu->pending_ready_count = 0U;
    FSP_CRITICAL_SECTION_EXIT;
    read_sample(p_acc_g, p_gyro_rad_s, p_temp_c);
}

bool imu_try_read_sample(imu_runtime_t * p_imu,
                         imu_read_sample_fn_t read_sample,
                         icm42688Float3_t * p_acc_g,
                         icm42688Float3_t * p_gyro_rad_s,
                         float * p_temp_c,
                         uint32_t * p_sample_time_us,
                         uint32_t * p_ready_count,
                         imu_timebase_t * p_timebase)
{
    /* 正常运行阶段保持非阻塞：如果这轮还没有新中断，就直接跳过这次读取。 */
    FSP_CRITICAL_SECTION_DEFINE;
    uint16_t sample_ready_count;
    uint32_t sample_time_us;

    FSP_CRITICAL_SECTION_ENTER;
    sample_ready_count = p_imu->pending_ready_count;
    sample_time_us = p_imu->data_ready_time_us;
    p_imu->pending_ready_count = 0U;
    FSP_CRITICAL_SECTION_EXIT;

    if (0U == sample_ready_count)
    {
        return false;
    }

    read_sample(p_acc_g, p_gyro_rad_s, p_temp_c);

    if (NULL != p_sample_time_us)
    {
        (void) p_timebase;
        *p_sample_time_us = sample_time_us;
    }

    if (NULL != p_ready_count)
    {
        *p_ready_count = (uint32_t) sample_ready_count;
    }

    return true;
}

static float imu_update_filtered_temperature(imu_runtime_t * p_imu, float temperature_c)
{
    p_imu->current_temperature_c = temperature_c;

    if (!p_imu->has_filtered_temperature)
    {
        p_imu->filtered_temperature_c = temperature_c;
        p_imu->has_filtered_temperature = true;
    }
    else
    {
        p_imu->filtered_temperature_c += IMU_TEMP_COMP_TEMP_ALPHA *
                                         (temperature_c - p_imu->filtered_temperature_c);
    }

    return p_imu->filtered_temperature_c;
}

static bool imu_can_learn_temperature_compensation(icm42688Float3_t const * p_acc_g,
                                                   icm42688Float3_t const * p_corrected_gyro_rad_s)
{
    float acc_norm;
    float gyro_norm;

    if ((NULL == p_acc_g) || (NULL == p_corrected_gyro_rad_s))
    {
        return false;
    }

    acc_norm = imu_vector_norm(p_acc_g);
    gyro_norm = imu_vector_norm(p_corrected_gyro_rad_s);

    return ((acc_norm >= IMU_STATIC_ACC_MIN_G) &&
            (acc_norm <= IMU_STATIC_ACC_MAX_G) &&
            (gyro_norm <= IMU_TEMP_COMP_STATIC_GYRO_RAD_S_MAX));
}
