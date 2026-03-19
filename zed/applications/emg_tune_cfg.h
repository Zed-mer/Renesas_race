#ifndef EMG_TUNE_CFG_H
#define EMG_TUNE_CFG_H

/*
 * 当前版本按用户要求回到“老工程风格”的包络提取方式：
 * 1. 前端滤波继续沿用 ra6m5_ds_musle 的那组固定 Filter() 系数；
 * 2. 包络不再使用 attack / release 双时间常数；
 * 3. 包络统一改成“绝对值 + 滑动平均窗口”。
 *
 * 因此，对外开放的调参项也只保留一个：包络窗口长度。
 * 这个值越大，包络越平稳、延迟越大；越小，响应越快、抖动也会更明显。
 *
 * 老工程 ra6m5_ds_musle 使用的是 16 点滑动平均，
 * 所以这里默认值保持 16，便于和老工程直接对齐。
 */

/*
 * 包络滑动窗口长度
 * - 物理意义：对 fabs(filtered) 做多少点滑动平均。
 * - 默认值：16，对齐老工程 ENVELOPE_BUFFER_SIZE = 16。
 * - 调大：更平滑，静息更稳，但收缩和放松会更“拖”。
 * - 调小：响应更快，但更容易看到毛刺和抖动。
 * - 建议范围：4 ~ 64。
 */
#define EMG_TUNE_ENVELOPE_WINDOW_SIZE 16U

#endif /* EMG_TUNE_CFG_H */
