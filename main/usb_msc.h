#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 storage 分区磨损均衡与初始 FAT16 文件系统（需在 tinyusb_driver_install 之前调用）
 *
 * @return esp_err_t ESP_OK 表示初始化成功并已就绪供 USB MSC 访问
 */
esp_err_t usb_msc_storage_init(void);

#ifdef __cplusplus
}
#endif