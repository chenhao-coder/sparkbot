#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief ST7789 LCD 通路验证测试入口
 *
 * 依次执行: 颜色轮播、彩色条纹、棋盘格、RGB三色框、渐变填充、背光呼吸灯。
 * 循环运行, 每轮结束后延时2秒重新开始。
 */
void lcd_st7789_test_run(void);

#ifdef __cplusplus
}
#endif
