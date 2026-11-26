#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include "DAP_config.h"
#include "DAP.h"
#include "usbip_server.h"

static const char *TAG = "USBIP_SRV";

/* ==================== 1. 设备与端点参数配置 ==================== */
#define USBIP_PORT              3240
#define USBIP_VERSION           0x0111

#define DEV_VID                 0x0D28
#define DEV_PID                 0x020B
#define DEV_BCD                 0x0100

// CMSIS-DAP v2 (Vendor Specific WinUSB) 端点配置
#define DAP_INTF_NUM            0       // 单接口模式固定为 0
#define DAP_EP_OUT              0x01    // Bulk OUT 端点地址
#define DAP_EP_IN               0x81    // Bulk IN 端点地址
#define MS_OS_20_VENDOR_CODE    0x01    // 微软 OS 2.0 描述符请求码

/* ==================== 2. USB/IP 协议常量与结构体定义 ==================== */
#define OP_REQ_DEVLIST          0x8005
#define OP_REP_DEVLIST          0x0005
#define OP_REQ_IMPORT           0x8003
#define OP_REP_IMPORT           0x0003
#define USBIP_CMD_SUBMIT        0x00000001
#define USBIP_CMD_UNLINK        0x00000002
#define USBIP_RET_SUBMIT        0x00000003
#define USBIP_RET_UNLINK        0x00000004

typedef struct __attribute__((packed)) {
    uint16_t version;
    uint16_t command;
    uint32_t status;
} usbip_op_common_t;

typedef struct __attribute__((packed)) {
    char     path[256];
    char     busid[32];
    uint32_t busnum;
    uint32_t devnum;
    uint32_t speed;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bConfigurationValue;
    uint8_t  bNumConfigurations;
    uint8_t  bNumInterfaces;
} usbip_usb_device_t;

typedef struct __attribute__((packed)) {
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t padding;
} usbip_usb_interface_t;

typedef struct __attribute__((packed)) {
    uint32_t command;
    uint32_t seqnum;
    uint32_t devid;
    uint32_t direction; // 0: OUT, 1: IN
    uint32_t ep;
    uint32_t transfer_flags;
    uint32_t transfer_buffer_length;
    uint32_t start_frame;
    uint32_t number_of_packets;
    uint32_t interval;
    uint8_t  setup[8];
} usbip_cmd_submit_t;

typedef struct __attribute__((packed)) {
    uint32_t command;
    uint32_t seqnum;
    uint32_t devid;
    uint32_t direction;
    uint32_t ep;
    uint32_t status;
    uint32_t actual_length;
    uint32_t start_frame;
    uint32_t number_of_packets;
    uint32_t error_count;
    uint8_t  padding[8];
} usbip_ret_submit_t;

typedef struct __attribute__((packed)) {
    uint32_t command;
    uint32_t seqnum;
    uint32_t devid;
    uint32_t direction;
    uint32_t ep;
    int32_t  status;
    uint8_t  padding[24];
} usbip_ret_unlink_t;

/* ==================== 3. 标准 USB 描述符 (对齐 64 字节 Full-Speed & CMSIS-DAP v2) ==================== */
static const uint8_t s_dev_desc[18] = {
    0x12, 0x01,
    0x10, 0x02,                         // bcdUSB = 0x0210 (触发 Windows 读取 BOS & MS OS 2.0)
    0x00, 0x00, 0x00, 0x40,             // DeviceClass=0, MaxPacketSize0=64
    (DEV_VID & 0xFF), (DEV_VID >> 8),
    (DEV_PID & 0xFF), (DEV_PID >> 8),
    (DEV_BCD & 0xFF), (DEV_BCD >> 8),
    0x01, 0x02, 0x03, 0x01              // iManufacturer=1, iProduct=2, iSerialNumber=3, bNumConfigurations=1
};

// 配置描述符：单接口 WinUSB，端点包长 64 字节 (0x40, 0x00)
static const uint8_t s_cfg_desc[32] = {
    // Configuration Descriptor (9 bytes)
    0x09, 0x02, 0x20, 0x00, 0x01, 0x01, 0x00, 0x80, 0xFA,
    // Interface Descriptor (9 bytes)
    0x09, 0x04, DAP_INTF_NUM, 0x00, 0x02, 0xFF, 0x00, 0x00, 0x02,
    // Endpoint Bulk OUT (7 bytes) - wMaxPacketSize = 64 (0x0040)
    0x07, 0x05, DAP_EP_OUT, 0x02, 0x40, 0x00, 0x00,
    // Endpoint Bulk IN (7 bytes) - wMaxPacketSize = 64 (0x0040)
    0x07, 0x05, DAP_EP_IN,  0x02, 0x40, 0x00, 0x00
};

// BOS 描述符 (33 bytes = 0x21)
static const uint8_t s_bos_desc[33] = {
    0x05, 0x0F, 0x21, 0x00, 0x01,
    0x1C, 0x10, 0x05, 0x00,
    0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C,
    0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F,
    0x00, 0x00, 0x03, 0x06,
    0xA2, 0x00,
    MS_OS_20_VENDOR_CODE, 0x00
};

// MS OS 2.0 描述符集 (162 字节 = 0xA2)
static const uint8_t s_ms_os_20_desc[162] = {
    0x0A, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x03, 0x06,
    0xA2, 0x00,

    0x14, 0x00, 0x03, 0x00,
    'W','I','N','U','S','B',0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

    0x84, 0x00,
    0x04, 0x00,
    0x07, 0x00,
    0x2A, 0x00,
    'D',0,'e',0,'v',0,'i',0,'c',0,'e',0,'I',0,'n',0,'t',0,'e',0,'r',0,'f',0,'a',0,'c',0,'e',0,'G',0,'U',0,'I',0,'D',0,'s',0, 0x00,0x00,
    0x50, 0x00,
    '{',0,'C',0,'D',0,'B',0,'3',0,'B',0,'5',0,'A',0,'D',0,'-',0,'2',0,'9',0,'3',0,'B',0,'-',0,'4',0,'6',0,'6',0,'3',0,'-',0,
    'A',0,'A',0,'3',0,'6',0,'-',0,'1',0,'A',0,'A',0,'E',0,'4',0,'6',0,'4',0,'6',0,'3',0,'7',0,'7',0,'6',0,'}',0,
    0x00,0x00,
    0x00,0x00
};

/* ==================== 4. 辅助收发与状态管理 ==================== */
static SemaphoreHandle_t s_dap_mutex = NULL;
static uint8_t s_dap_resp_buf[DAP_PACKET_SIZE];
static uint32_t s_dap_resp_len = 0;

/* 记录最新挂起的 Bulk IN URB */
typedef struct {
    bool     active;
    uint32_t seqnum;
    uint32_t devid;
    uint32_t ep;
    uint32_t max_len;
} pending_in_urb_t;

static pending_in_urb_t s_pending_in = {0};

static int recv_exact(int sock, void *buf, size_t len)
{
    size_t total = 0;
    uint8_t *p = (uint8_t *)buf;
    while (total < len) {
        int r = recv(sock, p + total, len - total, 0);
        if (r <= 0) return r;
        total += r;
    }
    return (int)total;
}

static int send_exact(int sock, const void *buf, size_t len)
{
    size_t total = 0;
    const uint8_t *p = (const uint8_t *)buf;
    while (total < len) {
        int s = send(sock, p + total, len - total, 0);
        if (s <= 0) return s;
        total += s;
    }
    return (int)total;
}

static void fill_usbip_device_info(usbip_usb_device_t *dev)
{
    memset(dev, 0, sizeof(*dev));
    strcpy(dev->path, "/sys/devices/platform/esp32s3_dap/usb1/1-1");
    strcpy(dev->busid, "1-1");
    dev->busnum              = htonl(1);
    dev->devnum              = htonl(2);
    dev->speed               = htonl(2); // USB_SPEED_FULL (匹配 64 字节批量端点)
    dev->idVendor            = htons(DEV_VID);
    dev->idProduct           = htons(DEV_PID);
    dev->bcdDevice           = htons(DEV_BCD);
    dev->bDeviceClass        = 0x00;
    dev->bDeviceSubClass     = 0x00;
    dev->bDeviceProtocol     = 0x00;
    dev->bConfigurationValue = 1;
    dev->bNumConfigurations  = 1;
    dev->bNumInterfaces      = 1;
}

/* ==================== 5. 阶段一：处理 DEVLIST 与 IMPORT 握手 ==================== */
static bool handle_op_handshake(int sock)
{
    usbip_op_common_t req;
    if (recv_exact(sock, &req, sizeof(req)) <= 0) return false;

    uint16_t cmd = ntohs(req.command);
    if (cmd == OP_REQ_DEVLIST) {
        ESP_LOGI(TAG, "收到设备列表查询 (OP_REQ_DEVLIST)");
        struct __attribute__((packed)) {
            usbip_op_common_t hdr;
            uint32_t ndev;
            usbip_usb_device_t dev;
            usbip_usb_interface_t intf;
        } rep;

        rep.hdr.version = htons(USBIP_VERSION);
        rep.hdr.command = htons(OP_REP_DEVLIST);
        rep.hdr.status  = htonl(0);
        rep.ndev        = htonl(1);
        fill_usbip_device_info(&rep.dev);
        rep.intf.bInterfaceClass    = 0xFF;
        rep.intf.bInterfaceSubClass = 0x00;
        rep.intf.bInterfaceProtocol = 0x00;
        rep.intf.padding            = 0;

        send_exact(sock, &rep, sizeof(rep));
        uint8_t dummy;
        recv(sock, &dummy, 1, 0);
        return false;
    }
    else if (cmd == OP_REQ_IMPORT) {
        char busid[32];
        if (recv_exact(sock, busid, sizeof(busid)) <= 0) return false;
        ESP_LOGI(TAG, "收到挂载请求 (OP_REQ_IMPORT), busid=%.32s", busid);

        if (strncmp(busid, "1-1", 3) != 0) {
            ESP_LOGW(TAG, "请求的 busid 不匹配: %.32s", busid);
            usbip_op_common_t err_rep = {
                .version = htons(USBIP_VERSION),
                .command = htons(OP_REP_IMPORT),
                .status  = htonl(1)
            };
            send_exact(sock, &err_rep, sizeof(err_rep));
            return false;
        }

        struct __attribute__((packed)) {
            usbip_op_common_t hdr;
            usbip_usb_device_t dev;
        } rep;

        rep.hdr.version = htons(USBIP_VERSION);
        rep.hdr.command = htons(OP_REP_IMPORT);
        rep.hdr.status  = htonl(0);
        fill_usbip_device_info(&rep.dev);

        if (send_exact(sock, &rep, sizeof(rep)) <= 0) return false;
        ESP_LOGI(TAG, "设备 1-1 挂载成功，进入 Full-Speed (64B) URB 透传模式");
        return true;
    }

    ESP_LOGW(TAG, "未知 USB/IP 握手指令: 0x%04x", cmd);
    return false;
}

/* ==================== 6. 阶段二：处理 EP0 控制传输与 DAP Bulk 传输 ==================== */
static void make_utf16_str_desc(const char *ascii, uint8_t *out, uint32_t *out_len, uint32_t max_len)
{
    size_t slen = strlen(ascii);
    uint32_t total = 2 + slen * 2;
    if (total > max_len) total = max_len;
    out[0] = (uint8_t)total;
    out[1] = 0x03;
    for (size_t i = 0; i < (total - 2) / 2; i++) {
        out[2 + i * 2]     = (uint8_t)ascii[i];
        out[2 + i * 2 + 1] = 0x00;
    }
    *out_len = total;
}

static int handle_ep0_control(const uint8_t setup[8], uint8_t *resp_data, uint32_t max_len, uint32_t *actual_len)
{
    uint8_t  bmRequestType = setup[0];
    uint8_t  bRequest      = setup[1];
    uint16_t wValue        = setup[2] | (setup[3] << 8);
    uint16_t wIndex        = setup[4] | (setup[5] << 8);
    uint16_t wLength       = setup[6] | (setup[7] << 8);
    *actual_len = 0;

    if ((bmRequestType & 0x60) == 0x00) {
        if (bRequest == 0x00) { // GET_STATUS
            if (wLength >= 2 && max_len >= 2) {
                resp_data[0] = 0x00;
                resp_data[1] = 0x00;
                *actual_len = 2;
            }
            return 0;
        }
        else if (bRequest == 0x01 || bRequest == 0x03) {
            return 0;
        }
        else if (bRequest == 0x06) { // GET_DESCRIPTOR
            uint8_t desc_type  = wValue >> 8;
            uint8_t desc_index = wValue & 0xFF;
            const uint8_t *src = NULL;
            uint32_t src_len = 0;

            if (desc_type == 0x01) {
                src = s_dev_desc; src_len = sizeof(s_dev_desc);
            } else if (desc_type == 0x02) {
                src = s_cfg_desc; src_len = sizeof(s_cfg_desc);
            } else if (desc_type == 0x0F) {
                src = s_bos_desc; src_len = sizeof(s_bos_desc);
            } else if (desc_type == 0x03) {
                if (desc_index == 0) {
                    static const uint8_t lang[4] = {0x04, 0x03, 0x09, 0x04};
                    src = lang; src_len = 4;
                } else if (desc_index == 1) {
                    make_utf16_str_desc("ARM", resp_data, actual_len, max_len);
                } else if (desc_index == 2) {
                    make_utf16_str_desc("ESP32-S3 WiFi CMSIS-DAP v2", resp_data, actual_len, max_len);
                } else if (desc_index == 3) {
                    make_utf16_str_desc("ESP32S3_WIFI_DAP_020B", resp_data, actual_len, max_len);
                } else {
                    *actual_len = 0;
                    return 0;
                }
                if (src == NULL) {
                    if (*actual_len > wLength) *actual_len = wLength;
                    return 0;
                }
            } else if (desc_type == 0x06) {
                return -32; // Full-Speed 设备对 DEVICE_QUALIFIER 返回 STALL
            }

            if (src != NULL) {
                uint32_t copy_len = (src_len < wLength) ? src_len : wLength;
                if (copy_len > max_len) copy_len = max_len;
                memcpy(resp_data, src, copy_len);
                *actual_len = copy_len;
                return 0;
            }
            return -32;
        }
        else if (bRequest == 0x08) {
            if (wLength >= 1 && max_len >= 1) {
                resp_data[0] = 1;
                *actual_len = 1;
            }
            return 0;
        }
        else if (bRequest == 0x09 || bRequest == 0x0B || bRequest == 0x05) {
            return 0;
        }
    }
    else if (bmRequestType == 0xC0 && bRequest == MS_OS_20_VENDOR_CODE && wIndex == 0x07) {
        ESP_LOGI(TAG, "响应 MS OS 2.0 描述符请求 (wLength=%u)", wLength);
        uint32_t copy_len = (sizeof(s_ms_os_20_desc) < wLength) ? sizeof(s_ms_os_20_desc) : wLength;
        if (copy_len > max_len) copy_len = max_len;
        memcpy(resp_data, s_ms_os_20_desc, copy_len);
        *actual_len = copy_len;
        return 0;
    }

    return 0;
}

static bool send_ret_submit_pkt(int sock, uint32_t seqnum, uint32_t devid, uint32_t dir,
                                uint32_t ep, int32_t status, const uint8_t *data, uint32_t len)
{
    usbip_ret_submit_t ret = {
        .command           = htonl(USBIP_RET_SUBMIT),
        .seqnum            = htonl(seqnum),
        .devid             = htonl(devid),
        .direction         = htonl(dir),
        .ep                = htonl(ep),
        .status            = htonl((uint32_t)status),
        .actual_length     = htonl(len),
        .start_frame       = 0,
        .number_of_packets = 0,
        .error_count       = 0,
    };

    if (send_exact(sock, &ret, sizeof(ret)) <= 0) return false;
    if (dir == 1 && len > 0 && data != NULL) {
        if (send_exact(sock, data, len) <= 0) return false;
    }
    return true;
}

static void handle_urb_loop(int sock)
{
    static uint8_t rx_payload[512];
    static uint8_t tx_payload[512];

    memset(&s_pending_in, 0, sizeof(s_pending_in));

    while (1) {
        uint32_t cmd_be;
        if (recv_exact(sock, &cmd_be, 4) <= 0) break;
        uint32_t command = ntohl(cmd_be);

        if (command == USBIP_CMD_SUBMIT) {
            usbip_cmd_submit_t sub;
            sub.command = cmd_be;
            if (recv_exact(sock, ((uint8_t *)&sub) + 4, sizeof(sub) - 4) <= 0) break;

            uint32_t seqnum = ntohl(sub.seqnum);
            uint32_t devid  = ntohl(sub.devid);
            uint32_t dir    = ntohl(sub.direction); // 0: OUT, 1: IN
            uint32_t ep     = ntohl(sub.ep);
            uint32_t len    = ntohl(sub.transfer_buffer_length);

            if (len > sizeof(rx_payload)) {
                ESP_LOGE(TAG, "URB 载荷过大: %lu", (unsigned long)len);
                break;
            }

            if (dir == 0 && len > 0) {
                if (recv_exact(sock, rx_payload, len) <= 0) break;
            }

            /* 1. 控制端点 EP0 */
            if (ep == 0) {
                int32_t  urb_status = 0;
                uint32_t actual_len = 0;
                if (dir == 1) {
                    urb_status = handle_ep0_control(sub.setup, tx_payload, sizeof(tx_payload), &actual_len);
                } else {
                    actual_len = len;
                }
                if (!send_ret_submit_pkt(sock, seqnum, devid, dir, ep, urb_status, tx_payload, actual_len)) {
                    break;
                }
            }
            /* 2. Bulk OUT：接收 CMSIS-DAP 指令 */
            else if (ep == (DAP_EP_OUT & 0x0F) && dir == 0) {
                if (len > DAP_PACKET_SIZE) {
                    ESP_LOGE(TAG, "Bulk OUT 超过 DAP_PACKET_SIZE(%u): %lu", DAP_PACKET_SIZE, (unsigned long)len);
                    break;
                }

                // 立即回复 OUT URB 完成确认
                if (!send_ret_submit_pkt(sock, seqnum, devid, 0, ep, 0, NULL, len)) {
                    break;
                }

                // 执行 CMSIS-DAP 指令
                uint32_t resp_len = 0;
                if (xSemaphoreTake(s_dap_mutex, portMAX_DELAY) == pdTRUE) {
                    memset(s_dap_resp_buf, 0, sizeof(s_dap_resp_buf));
                    s_dap_resp_len = DAP_ProcessCommand(rx_payload, s_dap_resp_buf) & 0xFFFF;
                    resp_len = s_dap_resp_len;

                    // 【调试日志】打印所有 DAP 收发关键头部字节与 URB 序号
                    ESP_LOGI(TAG, "[OUT seq=%lu len=%lu] %02X %02X %02X %02X %02X %02X | [RESP len=%lu] %02X %02X %02X %02X",
                             (unsigned long)seqnum, (unsigned long)len,
                             rx_payload[0], rx_payload[1], rx_payload[2], rx_payload[3], rx_payload[4], rx_payload[5],
                             (unsigned long)resp_len,
                             s_dap_resp_buf[0], s_dap_resp_buf[1], s_dap_resp_buf[2], s_dap_resp_buf[3]);

                    xSemaphoreGive(s_dap_mutex);
                }

                // 借用最新挂起的 Bulk IN URB 发回响应
                if (s_pending_in.active && resp_len > 0) {
                    uint32_t copy_len = (resp_len < s_pending_in.max_len) ? resp_len : s_pending_in.max_len;
                    if (xSemaphoreTake(s_dap_mutex, portMAX_DELAY) == pdTRUE) {
                        memcpy(tx_payload, s_dap_resp_buf, copy_len);
                        s_dap_resp_len = 0;
                        xSemaphoreGive(s_dap_mutex);
                    }
                    ESP_LOGI(TAG, "[IN-PEND Reply seq=%lu len=%lu]", (unsigned long)s_pending_in.seqnum, (unsigned long)copy_len);
                    if (!send_ret_submit_pkt(sock, s_pending_in.seqnum, s_pending_in.devid, 1,
                                             s_pending_in.ep, 0, tx_payload, copy_len)) {
                        break;
                    }
                    s_pending_in.active = false;
                } else if (resp_len > 0) {
                    ESP_LOGW(TAG, "[IN-WAIT] 尚无挂起 IN URB，缓存响应等待主机读取");
                }
            }
            /* 3. Bulk IN：请求读取 CMSIS-DAP 响应 */
            else if (ep == (DAP_EP_IN & 0x0F) && dir == 1) {
                uint32_t copy_len = 0;
                if (xSemaphoreTake(s_dap_mutex, portMAX_DELAY) == pdTRUE) {
                    if (s_dap_resp_len > 0) {
                        copy_len = (s_dap_resp_len < len) ? s_dap_resp_len : len;
                        memcpy(tx_payload, s_dap_resp_buf, copy_len);
                        s_dap_resp_len = 0;
                    }
                    xSemaphoreGive(s_dap_mutex);
                }

                if (copy_len > 0) {
                    ESP_LOGI(TAG, "[IN-IMM Reply seq=%lu len=%lu]", (unsigned long)seqnum, (unsigned long)copy_len);
                    if (!send_ret_submit_pkt(sock, seqnum, devid, 1, ep, 0, tx_payload, copy_len)) {
                        break;
                    }
                } else {
                    if (s_pending_in.active) {
                        ESP_LOGW(TAG, "[IN-OVERWRITE] 旧挂起 seq=%lu 被新 seq=%lu 覆盖",
                                 (unsigned long)s_pending_in.seqnum, (unsigned long)seqnum);
                    } else {
                        ESP_LOGI(TAG, "[IN-PEND] 挂起等待 seq=%lu max_len=%lu", (unsigned long)seqnum, (unsigned long)len);
                    }
                    s_pending_in.active  = true;
                    s_pending_in.seqnum  = seqnum;
                    s_pending_in.devid   = devid;
                    s_pending_in.ep      = ep;
                    s_pending_in.max_len = len;
                }
            }
        }
        else if (command == USBIP_CMD_UNLINK) {
            uint8_t unlink_rest[44];
            if (recv_exact(sock, unlink_rest, sizeof(unlink_rest)) <= 0) break;
            uint32_t seqnum_be, devid_be, dir_be, ep_be, unlink_seq_be;
            memcpy(&seqnum_be,     unlink_rest + 0,  4);
            memcpy(&devid_be,      unlink_rest + 4,  4);
            memcpy(&dir_be,        unlink_rest + 8,  4);
            memcpy(&ep_be,         unlink_rest + 12, 4);
            memcpy(&unlink_seq_be, unlink_rest + 16, 4);

            uint32_t target_seq = ntohl(unlink_seq_be);
            ESP_LOGW(TAG, "[UNLINK] 取消 target_seq=%lu (当前挂起 seq=%lu)",
                     (unsigned long)target_seq, (unsigned long)(s_pending_in.active ? s_pending_in.seqnum : 0));

            if (s_pending_in.active && target_seq == s_pending_in.seqnum) {
                s_pending_in.active = false;
            }

            usbip_ret_unlink_t ret = {
                .command   = htonl(USBIP_RET_UNLINK),
                .seqnum    = seqnum_be,
                .devid     = devid_be,
                .direction = dir_be,
                .ep        = ep_be,
                .status    = htonl((uint32_t)-104),
            };
            if (send_exact(sock, &ret, sizeof(ret)) <= 0) break;
        }
        else {
            ESP_LOGW(TAG, "未知 URB 命令: 0x%08lx", (unsigned long)command);
            break;
        }
    }

    s_pending_in.active = false;
}

/* ==================== 7. TCP Server 主任务 ==================== */
static void usbip_server_task(void *pvParameters)
{
    struct sockaddr_in server_addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(USBIP_PORT),
    };

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "创建监听 Socket 失败");
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "绑定端口 %d 失败", USBIP_PORT);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    listen(listen_sock, 2);
    ESP_LOGI(TAG, "USB/IP Server 已启动，监听端口: %d", USBIP_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (client_sock < 0) continue;

        int nodelay = 1;
        setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        if (xSemaphoreTake(s_dap_mutex, portMAX_DELAY) == pdTRUE) {
            s_dap_resp_len = 0;
            xSemaphoreGive(s_dap_mutex);
        }

        if (handle_op_handshake(client_sock)) {
            handle_urb_loop(client_sock);
        }

        ESP_LOGI(TAG, "USB/IP 客户端连接结束");
        close(client_sock);
    }
}

void usbip_server_start(void)
{
    if (s_dap_mutex == NULL) {
        s_dap_mutex = xSemaphoreCreateMutex();
    }
    xTaskCreatePinnedToCore(usbip_server_task, "usbip_srv", 6144, NULL, 5, NULL, 1);
}