#include "imu_runtime.h"
#include "imu_math.h"

#define MAHONY_KP  2.5f
#define MAHONY_KI  0.08f

void imu_runtime_reset(imu_runtime_t * p_imu)
{
    if (NULL == p_imu)
    {
        return;
    }

    imu_quaternion_identity(&p_imu->quat);
    p_imu->gyro_bias.x = 0.0f;
    p_imu->gyro_bias.y = 0.0f;
    p_imu->gyro_bias.z = 0.0f;
    p_imu->mahony_integral.x = 0.0f;
    p_imu->mahony_integral.y = 0.0f;
    p_imu->mahony_integral.z = 0.0f;
    p_imu->last_sample_time_us = 0U;
    p_imu->data_ready = false;
}

void imu_timebase_init(imu_timebase_t * p_timebase)
{
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

float imu_calc_dt_sec(imu_runtime_t * p_imu, uint32_t sample_time_us)
{
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
            dt_sec = IMU_SAMPLE_DT_DEFAULT_SEC;
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
    uint32_t         attempts = 0U;
    uint32_t         valid_samples = 0U;
    icm42688Float3_t gyro_sum = {0.0f, 0.0f, 0.0f};

    while ((attempts < IMU_CALIBRATION_MAX_ATTEMPTS) && (valid_samples < IMU_CALIBRATION_SAMPLES))
    {
        icm42688Float3_t acc_g = {0.0f, 0.0f, 0.0f};
        icm42688Float3_t gyro_rad_s = {0.0f, 0.0f, 0.0f};
        float            acc_norm;

        if (NULL != wait_hook)
        {
            wait_hook(p_wait_context, imu_time_now_us(p_timebase));
        }

        imu_read_sample_blocking(p_imu, read_sample, &acc_g, &gyro_rad_s);
        attempts++;

        acc_norm = imu_vector_norm(&acc_g);
        if ((acc_norm < IMU_STATIC_ACC_MIN_G) || (acc_norm > IMU_STATIC_ACC_MAX_G))
        {
            continue;
        }

        gyro_sum.x += gyro_rad_s.x;
        gyro_sum.y += gyro_rad_s.y;
        gyro_sum.z += gyro_rad_s.z;
        valid_samples++;
    }

    if (0U == valid_samples)
    {
        p_imu->gyro_bias.x = 0.0f;
        p_imu->gyro_bias.y = 0.0f;
        p_imu->gyro_bias.z = 0.0f;
        return 0U;
    }

    p_imu->gyro_bias.x = gyro_sum.x / (float) valid_samples;
    p_imu->gyro_bias.y = gyro_sum.y / (float) valid_samples;
    p_imu->gyro_bias.z = gyro_sum.z / (float) valid_samples;

    return valid_samples;
}

void imu_read_sample_blocking(imu_runtime_t * p_imu,
                              imu_read_sample_fn_t read_sample,
                              icm42688Float3_t * p_acc_g,
                              icm42688Float3_t * p_gyro_rad_s)
{
    uint32_t wait_ms = 0U;

    while ((!p_imu->data_ready) && (wait_ms < IMU_IRQ_WAIT_TIMEOUT_MS))
    {
        R_BSP_SoftwareDelay(1U, BSP_DELAY_UNITS_MILLISECONDS);
        wait_ms++;
    }

    p_imu->data_ready = false;
    read_sample(p_acc_g, p_gyro_rad_s);
}

bool imu_try_read_sample(imu_runtime_t * p_imu,
                         imu_read_sample_fn_t read_sample,
                         icm42688Float3_t * p_acc_g,
                         icm42688Float3_t * p_gyro_rad_s,
                         uint32_t * p_sample_time_us,
                         imu_timebase_t * p_timebase)
{
    if (!p_imu->data_ready)
    {
        return false;
    }

    p_imu->data_ready = false;
    read_sample(p_acc_g, p_gyro_rad_s);

    if (NULL != p_sample_time_us)
    {
        *p_sample_time_us = imu_time_now_us(p_timebase);
    }

    return true;
}
