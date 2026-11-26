#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <stddef.h>
#include <stdint.h>
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动基于 80 端口的 Web 控制台与 WebSocket 串口透传服务
 * @note  复用了 main.c 中已初始化的 UART_NUM_1 (115200bps)
 * @return httpd_handle_t 服务器句柄
 */
httpd_handle_t start_web_server(void);

/**
 * @brief 停止 Web 控制台服务
 */
void stop_web_server(httpd_handle_t server);

/**
 * @brief 将从 UART1 读取到的数据异步推送给已连接的网页 xterm.js 终端
 * @param data 数据指针
 * @param len  数据长度
 */
void web_server_send_uart_data(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* WEB_SERVER_H */