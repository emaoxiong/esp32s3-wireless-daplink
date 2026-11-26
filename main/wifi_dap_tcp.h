#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 避开 USBIP 的 3240 端口，使用 3241 作为 elaphureLink 端口
#define ELAPHURE_LINK_PORT          3240

// 与 components/cmsis_dap/DAP_config.h 中的 DAP_PACKET_SIZE (64) 保持一致
#define ELAPHURE_DAP_PACKET_SIZE    64

/**
 * @brief 启动 elaphureLink TCP 服务端任务
 * 
 * @return esp_err_t ESP_OK 表示启动成功
 */
esp_err_t wifi_dap_tcp_start(void);

/**
 * @brief 停止 elaphureLink TCP 服务端任务
 */
void wifi_dap_tcp_stop(void);

#ifdef __cplusplus
}
#endif