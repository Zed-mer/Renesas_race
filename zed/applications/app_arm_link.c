#include "app_arm_link.h"
#include "drv_MG996.h"
#include "hal_data.h"
#include <stdbool.h>

#define ARM_SERVO_SPEED_STEP   0.5f

/* ====== 每个物理舵机的零位补偿，后面按实际慢慢调 ====== */
#define CODE_SERVO5_OFFSET     0.0f   /* 底盘 */
#define CODE_SERVO4_OFFSET     0.0f   /* 大臂前半段 */
#define CODE_SERVO3_OFFSET     0.0f   /* 大臂后半段补偿 */
#define CODE_SERVO2_OFFSET     0.0f   /* 小臂肘关节 */
#define CODE_SERVO1_OFFSET     0.0f   /* 手腕 */

/* ====== 是否反向，0=同向，1=反向 ====== */
#define CODE_SERVO5_REVERSE    0
#define CODE_SERVO4_REVERSE    0
#define CODE_SERVO3_REVERSE    0
#define CODE_SERVO2_REVERSE    0
#define CODE_SERVO1_REVERSE    0

/* ====== 大臂分段补偿参数 ======
 * 上位机大臂 0~90 由舵机4承担
 * 上位机大臂 90~180 由舵机3补偿剩余角度
 */
#define SHOULDER_SPLIT_DEG     80.0f

/* 当大臂<=90度时，舵机3保持在这个基准位 */
#define SERVO3_SHOULDER_BASE   90.0f

static float clampf(float x, float min_v, float max_v)
{
    if (x < min_v) return min_v;
    if (x > max_v) return max_v;
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

/* 大臂角度分段映射：
 * shoulder_deg: 上位机大臂角
 * out_servo4: 代码舵机4
 * out_servo3: 代码舵机3
 */
static void map_shoulder_split(float shoulder_deg, float *out_servo4, float *out_servo3)
{
    float s4;
    float s3;

    shoulder_deg = clampf(shoulder_deg, 0.0f, 180.0f);

    if (shoulder_deg <= SHOULDER_SPLIT_DEG)
    {
        /* 0~90度：舵机4工作，舵机3保持基准 */
        s4 = 90.0f - shoulder_deg;  // 反向映射：0度对应90度，90度对应0度
        s3 = SERVO3_SHOULDER_BASE;
    }
    else
    {
        /* 90~180度：舵机4顶到90，舵机3补偿后半段 */
        s4 = 10.0f;
        s3 = SERVO3_SHOULDER_BASE + (shoulder_deg - SHOULDER_SPLIT_DEG);
    }

    s4 = apply_reverse_and_offset(s4, CODE_SERVO4_REVERSE, CODE_SERVO4_OFFSET);
    s3 = apply_reverse_and_offset(s3, CODE_SERVO3_REVERSE, CODE_SERVO3_OFFSET);

    *out_servo4 = clampf(s4, 0.0f, 180.0f);
    *out_servo3 = clampf(s3, 0.0f, 180.0f);
}

void arm_link_init(void)
{
    Servo_Init_All();

    R_GPT_Open(g_timer4.p_ctrl, g_timer4.p_cfg);
    R_GPT_Start(g_timer4.p_ctrl);
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

    /* 底盘：上位机 -> 代码舵机5 */
    angle_code_5 = apply_reverse_and_offset(base_deg, CODE_SERVO5_REVERSE, CODE_SERVO5_OFFSET);
    angle_code_5 = clampf(angle_code_5, 0.0f, 180.0f);

    /* 大臂：上位机 -> 代码舵机4 + 代码舵机3 分段补偿 */
    map_shoulder_split(shoulder_deg, &angle_code_4, &angle_code_3);

    /* 小臂肘关节：
     * 上位机角度 = 90 - 舵机2角度
     * => 舵机2角度 = 90 - 上位机角度
     */
    angle_code_2 = 90.0f - elbow_deg;
    angle_code_2 = apply_reverse_and_offset(angle_code_2, CODE_SERVO2_REVERSE, CODE_SERVO2_OFFSET);
    angle_code_2 = clampf(angle_code_2, 0.0f, 180.0f);

    /* 手腕：上位机 -> 代码舵机1 */
    angle_code_1 = 180.0f - wrist_deg;
    angle_code_1 = apply_reverse_and_offset(angle_code_1, CODE_SERVO1_REVERSE, CODE_SERVO1_OFFSET);
    angle_code_1 = clampf(angle_code_1, 0.0f, 180.0f);

    Servo_SetTargetAngle(5, angle_code_5, ARM_SERVO_SPEED_STEP);
    Servo_SetTargetAngle(4, angle_code_4, ARM_SERVO_SPEED_STEP);
    Servo_SetTargetAngle(3, angle_code_3, ARM_SERVO_SPEED_STEP);
    Servo_SetTargetAngle(2, angle_code_2, ARM_SERVO_SPEED_STEP);
    Servo_SetTargetAngle(1, angle_code_1, ARM_SERVO_SPEED_STEP);
}
void arm_apply_imu_pose_to_servos(const imu_servo_pose_t *pose)
{
    if (pose == NULL)
    {
        return;
    }

    arm_pose_calib_test((float)pose->hY_deg,
                        (float)pose->hZ_deg,
                        (float)pose->eZ_deg,
                        (float)pose->wX_deg);
}
