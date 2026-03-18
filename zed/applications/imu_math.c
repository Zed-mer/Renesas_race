#include "imu_math.h"
#include <math.h>

/*
 * 数学工具层：
 * 这里提供向量和四元数的基础运算，供姿态融合和标定映射复用。
 */

float imu_vector_norm(icm42688Float3_t const * p_vector)
{
    return sqrtf((p_vector->x * p_vector->x) + (p_vector->y * p_vector->y) + (p_vector->z * p_vector->z));
}

float imu_vector_dot(icm42688Float3_t const * p_left, icm42688Float3_t const * p_right)
{
    return (p_left->x * p_right->x) + (p_left->y * p_right->y) + (p_left->z * p_right->z);
}

icm42688Float3_t imu_vector_cross(icm42688Float3_t const * p_left, icm42688Float3_t const * p_right)
{
    icm42688Float3_t result = {0.0f, 0.0f, 0.0f};

    result.x = (p_left->y * p_right->z) - (p_left->z * p_right->y);
    result.y = (p_left->z * p_right->x) - (p_left->x * p_right->z);
    result.z = (p_left->x * p_right->y) - (p_left->y * p_right->x);

    return result;
}

bool imu_vector_normalize_unit(icm42688Float3_t * p_vector)
{
    float norm;

    if (NULL == p_vector)
    {
        return false;
    }

    /* 过小的向量在后续做归一化和投影时会数值不稳定，这里直接判失败。 */
    norm = imu_vector_norm(p_vector);
    if (norm <= IMU_VECTOR_EPSILON)
    {
        return false;
    }

    p_vector->x /= norm;
    p_vector->y /= norm;
    p_vector->z /= norm;

    return true;
}

float imu_clampf(float value, float min_value, float max_value)
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

void imu_quaternion_normalize(Quaternion_t * p_quat)
{
    /* 四元数在连续积分和连乘后会有数值漂移，这里强制拉回单位四元数。 */
    float norm = sqrtf((p_quat->q0 * p_quat->q0) + (p_quat->q1 * p_quat->q1) +
                       (p_quat->q2 * p_quat->q2) + (p_quat->q3 * p_quat->q3));

    if (norm <= 0.0f)
    {
        p_quat->q0 = 1.0f;
        p_quat->q1 = 0.0f;
        p_quat->q2 = 0.0f;
        p_quat->q3 = 0.0f;
        return;
    }

    p_quat->q0 /= norm;
    p_quat->q1 /= norm;
    p_quat->q2 /= norm;
    p_quat->q3 /= norm;
}

void imu_quaternion_identity(Quaternion_t * p_quat)
{
    if (NULL == p_quat)
    {
        return;
    }

    p_quat->q0 = 1.0f;
    p_quat->q1 = 0.0f;
    p_quat->q2 = 0.0f;
    p_quat->q3 = 0.0f;
}

Quaternion_t imu_quaternion_multiply(Quaternion_t const * p_left, Quaternion_t const * p_right)
{
    /* 四元数连乘表示旋转组合，在本项目里常用于姿态修正与坐标系变换。 */
    /* 四元数连乘用于组合两个旋转，结果表示按既定顺序叠加后的总旋转。 */
    Quaternion_t result = {0.0f, 0.0f, 0.0f, 0.0f};

    result.q0 = (p_left->q0 * p_right->q0) - (p_left->q1 * p_right->q1) -
                (p_left->q2 * p_right->q2) - (p_left->q3 * p_right->q3);
    result.q1 = (p_left->q0 * p_right->q1) + (p_left->q1 * p_right->q0) +
                (p_left->q2 * p_right->q3) - (p_left->q3 * p_right->q2);
    result.q2 = (p_left->q0 * p_right->q2) - (p_left->q1 * p_right->q3) +
                (p_left->q2 * p_right->q0) + (p_left->q3 * p_right->q1);
    result.q3 = (p_left->q0 * p_right->q3) + (p_left->q1 * p_right->q2) -
                (p_left->q2 * p_right->q1) + (p_left->q3 * p_right->q0);
    imu_quaternion_normalize(&result);

    return result;
}

Quaternion_t imu_quaternion_inverse(Quaternion_t const * p_quat)
{
    /* 对单位四元数来说，求逆等价于取共轭，计算量更小。 */
    Quaternion_t result = {0.0f, 0.0f, 0.0f, 0.0f};

    result.q0 = p_quat->q0;
    result.q1 = -p_quat->q1;
    result.q2 = -p_quat->q2;
    result.q3 = -p_quat->q3;
    imu_quaternion_normalize(&result);

    return result;
}

icm42688Float3_t imu_quaternion_rotate_vector(Quaternion_t const * p_quat, icm42688Float3_t const * p_vector)
{
    /* 直接用四元数旋转向量，避免额外构造旋转矩阵。 */
    icm42688Float3_t q_vector = {p_quat->q1, p_quat->q2, p_quat->q3};
    icm42688Float3_t uv = imu_vector_cross(&q_vector, p_vector);
    icm42688Float3_t uuv = imu_vector_cross(&q_vector, &uv);
    icm42688Float3_t result = {0.0f, 0.0f, 0.0f};

    uv.x *= (2.0f * p_quat->q0);
    uv.y *= (2.0f * p_quat->q0);
    uv.z *= (2.0f * p_quat->q0);
    uuv.x *= 2.0f;
    uuv.y *= 2.0f;
    uuv.z *= 2.0f;

    result.x = p_vector->x + uv.x + uuv.x;
    result.y = p_vector->y + uv.y + uuv.y;
    result.z = p_vector->z + uv.z + uuv.z;

    return result;
}

bool imu_quaternion_extract_axis_angle(Quaternion_t const * p_quat,
                                       icm42688Float3_t * p_axis,
                                       float * p_angle_deg)
{
    Quaternion_t     quat = {0.0f, 0.0f, 0.0f, 0.0f};
    icm42688Float3_t axis = {0.0f, 0.0f, 0.0f};
    float            axis_norm;

    if ((NULL == p_quat) || (NULL == p_axis) || (NULL == p_angle_deg))
    {
        return false;
    }

    /* 统一四元数符号，避免同一物理姿态出现正负两种等价表示。 */
    quat = *p_quat;
    imu_quaternion_normalize(&quat);
    if (quat.q0 < 0.0f)
    {
        quat.q0 = -quat.q0;
        quat.q1 = -quat.q1;
        quat.q2 = -quat.q2;
        quat.q3 = -quat.q3;
    }

    axis.x = quat.q1;
    axis.y = quat.q2;
    axis.z = quat.q3;
    axis_norm = imu_vector_norm(&axis);
    if (axis_norm <= IMU_VECTOR_EPSILON)
    {
        return false;
    }

    axis.x /= axis_norm;
    axis.y /= axis_norm;
    axis.z /= axis_norm;

    *p_axis = axis;
    *p_angle_deg = (2.0f * atan2f(axis_norm, quat.q0)) * IMU_RAD_TO_DEG;

    return true;
}

bool imu_quaternion_extract_rotation_vector_deg(Quaternion_t const * p_quat,
                                                icm42688Float3_t * p_rotation_vec_deg)
{
    /* 旋转向量把“旋转轴”和“旋转角”编码成一个向量，便于后续做基向量分解。 */
    Quaternion_t     quat = {0.0f, 0.0f, 0.0f, 0.0f};
    icm42688Float3_t quat_vector = {0.0f, 0.0f, 0.0f};
    float            vector_norm_deg;
    float            angle_deg;
    float            scale;

    if ((NULL == p_quat) || (NULL == p_rotation_vec_deg))
    {
        return false;
    }

    /* 把四元数换成“旋转轴乘角度”的旋转向量形式，便于后续做基向量分解。 */
    quat = *p_quat;
    imu_quaternion_normalize(&quat);
    if (quat.q0 < 0.0f)
    {
        quat.q0 = -quat.q0;
        quat.q1 = -quat.q1;
        quat.q2 = -quat.q2;
        quat.q3 = -quat.q3;
    }

    quat_vector.x = quat.q1;
    quat_vector.y = quat.q2;
    quat_vector.z = quat.q3;
    vector_norm_deg = imu_vector_norm(&quat_vector);
    if (vector_norm_deg <= IMU_VECTOR_EPSILON)
    {
        p_rotation_vec_deg->x = 0.0f;
        p_rotation_vec_deg->y = 0.0f;
        p_rotation_vec_deg->z = 0.0f;
        return true;
    }

    angle_deg = (2.0f * atan2f(vector_norm_deg, quat.q0)) * IMU_RAD_TO_DEG;
    scale = angle_deg / vector_norm_deg;
    p_rotation_vec_deg->x = quat_vector.x * scale;
    p_rotation_vec_deg->y = quat_vector.y * scale;
    p_rotation_vec_deg->z = quat_vector.z * scale;

    return true;
}
