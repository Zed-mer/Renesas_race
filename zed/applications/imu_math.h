#ifndef IMU_MATH_H
#define IMU_MATH_H

#include "imu_app_context.h"

float            imu_vector_norm(icm42688Float3_t const * p_vector);
float            imu_vector_dot(icm42688Float3_t const * p_left, icm42688Float3_t const * p_right);
icm42688Float3_t imu_vector_cross(icm42688Float3_t const * p_left, icm42688Float3_t const * p_right);
bool             imu_vector_normalize_unit(icm42688Float3_t * p_vector);
float            imu_clampf(float value, float min_value, float max_value);
void             imu_quaternion_normalize(Quaternion_t * p_quat);
void             imu_quaternion_identity(Quaternion_t * p_quat);
Quaternion_t     imu_quaternion_multiply(Quaternion_t const * p_left, Quaternion_t const * p_right);
Quaternion_t     imu_quaternion_inverse(Quaternion_t const * p_quat);
icm42688Float3_t imu_quaternion_rotate_vector(Quaternion_t const * p_quat, icm42688Float3_t const * p_vector);
bool             imu_quaternion_extract_axis_angle(Quaternion_t const * p_quat,
                                                   icm42688Float3_t * p_axis,
                                                   float * p_angle_deg);
bool             imu_quaternion_extract_rotation_vector_deg(Quaternion_t const * p_quat,
                                                            icm42688Float3_t * p_rotation_vec_deg);

#endif
