#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化并挂载 LittleFS (logfs 分区)
 *        自动扫描现有日志文件（支持 00001~99999 跨界回绕），续写未满 1MB 的最新文件或创建新文件
 * @return ESP_OK 成功，其他值表示挂载或文件打开失败
 */
esp_err_t logfs_init(void);

/**
 * @brief 写入串口数据到日志文件（线程安全）
 *        单文件达到 1MB 自动切分，最多保留 5 个文件，超出后自动删除最早的文件
 * @param data 待写入的字节数据指针
 * @param len  待写入的字节长度
 */
void logfs_write(const uint8_t *data, size_t len);

/**
 * @brief 强制将文件流缓冲区数据刷入 Flash（线程安全）
 *        建议在串口接收空闲超时（如 500ms 无新数据）或设备重启/关机前调用
 */
void logfs_flush(void);

#ifdef __cplusplus
}
#endif