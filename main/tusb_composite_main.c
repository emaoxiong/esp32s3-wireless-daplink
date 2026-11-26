/*
 * SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"

// 1. 在文件顶部头文件区域引入：
#include "DAP_config.h"
#include "DAP.h"

#include "driver/uart.h"
#include "tusb.h"

#include "wifi_sta.h"
#include "wifi_dap_tcp.h"
#include "usbip_server.h"

#include "web_server.h"

#include "usb_msc.h"

#include "logfs.h"

#define BRIDGE_UART_NUM     UART_NUM_1
#define BRIDGE_TXD_PIN      GPIO_NUM_17
#define BRIDGE_RXD_PIN      GPIO_NUM_18
#define BRIDGE_BUF_SIZE     2048

static void bridge_uart_init(void) {
    uart_config_t uart_config = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(BRIDGE_UART_NUM, BRIDGE_BUF_SIZE, BRIDGE_BUF_SIZE, 0, NULL, 0);
    uart_param_config(BRIDGE_UART_NUM, &uart_config);
    uart_set_pin(BRIDGE_UART_NUM, BRIDGE_TXD_PIN, BRIDGE_RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

static const char *TAG = "winusb_cdc_main";
static uint8_t cdc_rx_buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE + 1];

// 区分消息来源：CDC 串口 或 WinUSB (Vendor) 通道
typedef enum {
    MSG_SRC_CDC = 0,
    MSG_SRC_WINUSB,
} msg_src_t;

typedef struct {
    msg_src_t src;
    uint8_t buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE + 1];
    size_t buf_len;
    uint8_t itf;
} app_message_t;

static QueueHandle_t app_queue;

/**
 * @brief WinUSB (Vendor Bulk OUT) 接收回调
 */
void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;

    uint8_t vendor_buf[64];
    uint32_t count = tud_vendor_read(vendor_buf, sizeof(vendor_buf));
    if (count > 0) {
        app_message_t tx_msg = {
            .src = MSG_SRC_WINUSB,
            .buf_len = count,
            .itf = itf,
        };
        memcpy(tx_msg.buf, vendor_buf, count);
        xQueueSend(app_queue, &tx_msg, 0);
    }
}

/**
 * @brief CDC device RX callback
 */
void tinyusb_cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    // (void)event;
    // size_t rx_size = 0;

    // esp_err_t ret = tinyusb_cdcacm_read(itf, cdc_rx_buf, CONFIG_TINYUSB_CDC_RX_BUFSIZE, &rx_size);
    // if (ret == ESP_OK && rx_size > 0) {
    //     app_message_t tx_msg = {
    //         .src = MSG_SRC_CDC,
    //         .buf_len = rx_size,
    //         .itf = itf,
    //     };
    //     memcpy(tx_msg.buf, cdc_rx_buf, rx_size);
    //     xQueueSend(app_queue, &tx_msg, 0);
    // } else if (ret != ESP_OK) {
    //     ESP_LOGE(TAG, "CDC Read Error");
    // }

    (void)event;
    uint8_t rx_buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE];
    size_t rx_size = 0;

    while (tinyusb_cdcacm_read(itf, rx_buf, sizeof(rx_buf), &rx_size) == ESP_OK && rx_size > 0) {
        uart_write_bytes(BRIDGE_UART_NUM, (const char *)rx_buf, rx_size);
    }
}

/**
 * @brief CDC device line change callback (DTR / RTS)
 */
void tinyusb_cdc_line_state_changed_callback(int itf, cdcacm_event_t *event)
{
    int dtr = event->line_state_changed_data.dtr;
    int rts = event->line_state_changed_data.rts;
    ESP_LOGI(TAG, "Line state changed on channel %d: DTR:%d, RTS:%d", itf, dtr, rts);
}

// 在 app_main() 前面添加这 4 行 extern 声明：
extern tusb_desc_device_t const custom_desc_device;
extern uint8_t const custom_desc_configuration[];
extern char const *custom_string_desc_arr[];
extern const size_t custom_string_desc_count;

// 2. 上位机修改串口参数（波特率/数据位/停止位/校验位）回调
static void tinyusb_cdc_line_coding_callback(int itf, cdcacm_event_t *event)
{
    cdc_line_coding_t const *line_coding = event->line_coding_changed_data.p_line_coding;

    // 同步波特率（防止上位机刚枚举时下发 0 导致报错）
    if (line_coding->bit_rate > 0) {
        uart_set_baudrate(BRIDGE_UART_NUM, line_coding->bit_rate);
    }

    // 同步数据位
    uart_word_length_t data_bits = UART_DATA_8_BITS;
    switch (line_coding->data_bits) {
        case 5: data_bits = UART_DATA_5_BITS; break;
        case 6: data_bits = UART_DATA_6_BITS; break;
        case 7: data_bits = UART_DATA_7_BITS; break;
        case 8:
        default: data_bits = UART_DATA_8_BITS; break;
    }
    uart_set_word_length(BRIDGE_UART_NUM, data_bits);

    // 同步校验位
    uart_parity_t parity = UART_PARITY_DISABLE;
    const char *parity_str = "None";
    switch (line_coding->parity) {
        case 1: parity = UART_PARITY_ODD;  parity_str = "Odd";  break;
        case 2: parity = UART_PARITY_EVEN; parity_str = "Even"; break;
        case 0:
        default: parity = UART_PARITY_DISABLE; parity_str = "None"; break;
    }
    uart_set_parity(BRIDGE_UART_NUM, parity);

    // 同步停止位
    uart_stop_bits_t stop_bits = UART_STOP_BITS_1;
    const char *stop_str = "1";
    switch (line_coding->stop_bits) {
        case 1: stop_bits = UART_STOP_BITS_1_5; stop_str = "1.5"; break;
        case 2: stop_bits = UART_STOP_BITS_2;   stop_str = "2";   break;
        case 0:
        default: stop_bits = UART_STOP_BITS_1;  stop_str = "1";   break;
    }
    uart_set_stop_bits(BRIDGE_UART_NUM, stop_bits);

    // 打印上位机实际下发的串口配置参数（输出到 UART0 控制台，不影响 UART1 透传）
    ESP_LOGI("CDC_BRIDGE", "ITF[%d] Line Coding -> Baud: %" PRIu32 ", DataBits: %u, Parity: %s (%u), StopBits: %s (%u)",
             itf,
             line_coding->bit_rate,
             line_coding->data_bits,
             parity_str, line_coding->parity,
             stop_str, line_coding->stop_bits);
}

// 4. 目标芯片 -> PC 串口助手方向（硬件 UART1 RX -> USB CDC TX 后台任务）
static void uart_to_usb_task(void *arg)
{
    (void)arg;
    uint8_t buf[256];
    while (1) {
        int len = uart_read_bytes(BRIDGE_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(10));
        if (len > 0) {

            logfs_write(buf, len);

            // 1. 转发给 USB CDC（电脑串口助手）
            if (tud_cdc_n_connected(TINYUSB_CDC_ACM_0)) {
                size_t offset = 0;
                while (offset < (size_t)len) {
                    size_t queued = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, buf + offset, len - offset);
                    offset += queued;
                    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(10));
                    if (queued == 0) {
                        vTaskDelay(1);
                    }
                }
            }
            // 2. 同步推送给网页 xterm.js 终端
            web_server_send_uart_data(buf, len);
        }
    }
}

void app_main(void)
{
    app_queue = xQueueCreate(10, sizeof(app_message_t));
    assert(app_queue);
    app_message_t msg;

    ESP_LOGI(TAG, "USB Composite (WinUSB + CDC) initialization");

    const tinyusb_config_t tusb_cfg = {
            .device_descriptor = &custom_desc_device,
            .string_descriptor = custom_string_desc_arr,
            .string_descriptor_count = custom_string_desc_count,
            .external_phy = false,
    #if (TUD_OPT_HIGH_SPEED)
            .fs_configuration_descriptor = custom_desc_configuration,
            .hs_configuration_descriptor = NULL,
            .qualifier_descriptor = NULL,
    #else
            .configuration_descriptor = custom_desc_configuration,
    #endif
        };

    bridge_uart_init();

    DAP_Setup();

    // 2. 初始化 littlefs 日志系统 ✅ 就是这里
    ESP_ERROR_CHECK(logfs_init());

    // 2. 在启动 USB 驱动前，先初始化并格式化 storage 分区，移交存储控制权
    ESP_ERROR_CHECK(usb_msc_storage_init());

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));



    tinyusb_config_cdcacm_t acm_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .rx_unread_buf_sz = 64,
        .callback_rx = &tinyusb_cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = &tinyusb_cdc_line_coding_callback
    };
    ESP_ERROR_CHECK(tusb_cdc_acm_init(&acm_cfg));

    // 启动 UART1 -> USB 转发任务
    xTaskCreate(uart_to_usb_task, "uart_to_usb", 4096, NULL, 5, NULL);

    // ESP_ERROR_CHECK(tinyusb_cdcacm_register_callback(
    //                     TINYUSB_CDC_ACM_0,
    //                     CDC_EVENT_LINE_STATE_CHANGED,
    //                     &tinyusb_cdc_line_state_changed_callback));

    ESP_LOGI(TAG, "USB Composite initialization DONE");

    // 2. 延时 100ms 让 USB 完成枚举握手
    vTaskDelay(pdMS_TO_TICKS(100));

    // 3. 初始化 Wi-Fi STA（默认运行在 Core 0）
    wifi_init_sta();
    wifi_dap_tcp_start();
    start_web_server();
    // usbip_server_start();

    while (1) {
        if (xQueueReceive(app_queue, &msg, portMAX_DELAY)) {
            if (msg.buf_len == 0) {
                continue;
            }

            if (msg.src == MSG_SRC_CDC) {
                // 1. CDC 串口数据回环测试
                ESP_LOGI(TAG, "CDC RX (ch %d, %d bytes):", msg.itf, msg.buf_len);
                ESP_LOG_BUFFER_HEXDUMP(TAG, msg.buf, msg.buf_len, ESP_LOG_INFO);

                tinyusb_cdcacm_write_queue(msg.itf, msg.buf, msg.buf_len);
                esp_err_t err = tinyusb_cdcacm_write_flush(msg.itf, 0);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "CDC ACM write flush error: %s", esp_err_to_name(err));
                }
            } else if (msg.src == MSG_SRC_WINUSB) {
                // // 2. WinUSB (Vendor) 数据回环测试（后续此处替换为 DAP_ProcessCommand）
                // ESP_LOGI(TAG, "WinUSB RX (itf %d, %d bytes):", msg.itf, msg.buf_len);
                // ESP_LOG_BUFFER_HEXDUMP(TAG, msg.buf, msg.buf_len, ESP_LOG_INFO);

                // tud_vendor_n_write(msg.itf, msg.buf, msg.buf_len);
                // tud_vendor_n_flush(msg.itf);

                uint8_t dap_response[DAP_PACKET_SIZE];
                // 解析 PC 发来的 CMSIS-DAP 指令，返回值为需要回发的响应字节数
                uint32_t resp_len = DAP_ProcessCommand(msg.buf, dap_response);

                if (resp_len > 0) {
                    tud_vendor_n_write(msg.itf, dap_response, resp_len);
                    tud_vendor_n_flush(msg.itf);
                }

            }
        }
    }
}