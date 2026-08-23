#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_LCD_H_RES 240
#define BSP_LCD_V_RES 240

/*
 * 下面的类型/宏是为了对齐 esp-bsp 里 esp_lvgl_port 的用法而做的本地兼容层。
 * 工程里实际使用的是 esp_lvgl_adapter (esp_lvgl_port 的继任者),
 * 这里把 BSP 例子里用到的字段名映射到 adapter。
 */

/* 对应 esp_lvgl_port 的 lvgl_port_cfg_t (精简, 只保留例子用到的字段) */
typedef struct {
    uint32_t task_stack;      /*!< LVGL 任务栈大小 (字节), 对应 adapter 的 task_stack_size */
    int      task_affinity;   /*!< LVGL 任务亲和核 (-1 不绑定), 对应 adapter 的 task_core_id */
} lvgl_port_cfg_t;

/* 对应 esp_lvgl_port 的 ESP_LVGL_PORT_INIT_CONFIG() */
#define ESP_LVGL_PORT_INIT_CONFIG() \
    ((lvgl_port_cfg_t){ .task_stack = 8 * 1024, .task_affinity = -1 })

/* 对应 esp-bsp 的 bsp_display_cfg_t */
typedef struct {
    lvgl_port_cfg_t lvgl_port_cfg;  /*!< LVGL port 配置 */
    uint32_t        buffer_size;    /*!< 屏幕缓冲像素数 */
    uint32_t        trans_size;     /*!< 传输缓冲像素数 */
    bool            double_buffer;  /*!< 是否双缓冲 */
    struct {
        unsigned int buff_dma: 1;    /*!< 缓冲使用 DMA 内存 */
        unsigned int buff_spiram: 1; /*!< 缓冲使用 PSRAM */
    } flags;
} bsp_display_cfg_t;

/**
 * @brief 初始化显示 + LVGL (对应 bsp_display_start_with_config)
 */
esp_err_t bsp_display_start_with_config(const bsp_display_cfg_t *cfg);

/**
 * @brief 打开背光 (对应 bsp_display_backlight_on)
 */
esp_err_t bsp_display_backlight_on(void);

/**
 * @brief 获取 LVGL 锁 (对应 bsp_display_lock, 0 表示阻塞等待)
 */
esp_err_t bsp_display_lock(uint32_t timeout_ms);

/**
 * @brief 释放 LVGL 锁 (对应 bsp_display_unlock)
 */
void bsp_display_unlock(void);

#ifdef __cplusplus
}
#endif
