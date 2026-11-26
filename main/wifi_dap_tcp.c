#include "wifi_dap_tcp.h"
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "DAP_config.h"
#include "DAP.h"

static const char *TAG = "ELP_TCP";

static TaskHandle_t s_elp_task_handle = NULL;
static int s_listen_sock = -1;
static volatile bool s_is_running = false;

// ================= 官方 elaphureLink 协议常量 =================
#define EL_LINK_IDENTIFIER              0x8a656c70
#define EL_DAP_VERSION_ASYNC            0x10000
#define EL_COMMAND_HANDSHAKE            0x00000000

#define EL_VENDOR_COMMAND_PREFIX        0x88
#define EL_NATIVE_COMMAND_PASSTHROUGH   0x01
#define EL_VENDOR_SCOPE_ENTER           0x02
#define EL_VENDOR_SCOPE_EXIT            0x03

#define ID_DAP_EXECUTE_COMMANDS         0x7F

// 批量指令打包（0x7F）时响应可能超过 64 字节，缓冲区需足够容纳多条回复
#define EL_TCP_RX_BUF_SIZE              1500
#define EL_TCP_TX_BUF_SIZE              1500

typedef struct {
    uint32_t el_link_identifier;
    uint32_t command;
    uint32_t version;
} __attribute__((packed)) el_handshake_pkt_t;

static int recv_exact(int sock, uint8_t *buf, size_t size) {
    size_t total = 0;
    while (total < size) {
        int ret = recv(sock, buf + total, size - total, 0);
        if (ret <= 0) return ret;
        total += ret;
    }
    return (int)total;
}

static int send_exact(int sock, const uint8_t *buf, size_t size) {
    size_t total = 0;
    while (total < size) {
        int ret = send(sock, buf + total, size - total, 0);
        if (ret <= 0) return ret;
        total += ret;
    }
    return (int)total;
}

/**
 * @brief 执行标准 CMSIS-DAP 指令，原生支持 0x7F (DAP_ExecuteCommands) 批量拆包执行
 */
static uint16_t execute_dap_command_compat(const uint8_t *req, uint8_t *resp) {
    if (req[0] == ID_DAP_EXECUTE_COMMANDS) {
        uint8_t num_cmds = req[1];
        resp[0] = ID_DAP_EXECUTE_COMMANDS;
        resp[1] = 0; // 实际成功执行的指令计数

        uint16_t req_offset = 2;
        uint16_t resp_offset = 2;

        for (uint8_t i = 0; i < num_cmds; i++) {
            if (resp_offset + DAP_PACKET_SIZE > EL_TCP_TX_BUF_SIZE) {
                break;
            }
            // DAP_ProcessCommand 返回值: 高 16 位为消耗的请求字节数，低 16 位为生成的响应字节数
            uint32_t res = DAP_ProcessCommand(req + req_offset, resp + resp_offset);
            uint16_t req_used = (uint16_t)(res >> 16);
            uint16_t resp_gen = (uint16_t)(res & 0xFFFF);

            if (req_used == 0 || resp_gen == 0) {
                break;
            }
            req_offset += req_used;
            resp_offset += resp_gen;
            resp[1]++;
        }
        return resp_offset;
    }

    uint32_t dap_res = DAP_ProcessCommand(req, resp);
    return (uint16_t)(dap_res & 0xFFFF);
}

/**
 * @brief 处理单帧指令（兼容标准 CMSIS-DAP 指令与 0x88 Vendor 加速指令）
 */
static int process_single_el_packet(int sock, const uint8_t *req, uint8_t *resp, bool *is_async) {
    uint16_t send_len = 0;

    if (req[0] == EL_VENDOR_COMMAND_PREFIX) {
        uint8_t sub_cmd = req[1];
        switch (sub_cmd) {
            case EL_NATIVE_COMMAND_PASSTHROUGH: {
                uint16_t dap_resp_len = execute_dap_command_compat(req + 4, resp + 4);
                resp[0] = EL_VENDOR_COMMAND_PREFIX;
                resp[1] = 0x00;
                resp[2] = (dap_resp_len >> 8) & 0xFF;
                resp[3] = dap_resp_len & 0xFF;
                send_len = 4 + dap_resp_len;
                break;
            }
            case EL_VENDOR_SCOPE_ENTER: {
                *is_async = true;
                resp[0] = EL_VENDOR_COMMAND_PREFIX;
                resp[1] = 0x00;
                resp[2] = 0x00;
                resp[3] = 0x00;
                send_len = 4;
                break;
            }
            case EL_VENDOR_SCOPE_EXIT: {
                *is_async = false;
                resp[0] = EL_VENDOR_COMMAND_PREFIX;
                resp[1] = 0x00;
                resp[2] = 0x00;
                resp[3] = 0x00;
                send_len = 4;
                break;
            }
            default:
                ESP_LOGW(TAG, "Unknown vendor sub_cmd: 0x%02x", sub_cmd);
                return 0;
        }
    } else {
        send_len = execute_dap_command_compat(req, resp);
    }

    if (send_len > 0) {
        if (send_exact(sock, resp, send_len) != send_len) {
            return -1;
        }
    }
    return 0;
}

/**
 * @brief 处理粘包的多条 0x88 连续指令
 */
static int process_vendor_stream(int sock, uint8_t *rx_buf, int recved_len, uint8_t *tx_buf, bool *is_async) {
    int offset = 0;

    while (recved_len - offset >= 4) {
        uint16_t payload_len = ((uint16_t)rx_buf[offset + 2] << 8) | rx_buf[offset + 3];
        int packet_len = 4 + payload_len;

        if (offset + packet_len > recved_len) {
            break;
        }

        if (process_single_el_packet(sock, rx_buf + offset, tx_buf, is_async) < 0) {
            return -1;
        }
        offset += packet_len;
    }

    int remain_len = recved_len - offset;
    if (remain_len == 0) {
        return 0;
    }

    memmove(rx_buf, rx_buf + offset, remain_len);
    if (remain_len < 4) {
        if (recv_exact(sock, rx_buf + remain_len, 4 - remain_len) <= 0) return -1;
        remain_len = 4;
    }

    uint16_t payload_len = ((uint16_t)rx_buf[2] << 8) | rx_buf[3];
    int total_pkt_len = 4 + payload_len;
    if (total_pkt_len > EL_TCP_RX_BUF_SIZE) {
        ESP_LOGE(TAG, "Packet too large: %d", total_pkt_len);
        return -1;
    }

    if (total_pkt_len > remain_len) {
        if (recv_exact(sock, rx_buf + remain_len, total_pkt_len - remain_len) <= 0) return -1;
    }

    return process_single_el_packet(sock, rx_buf, tx_buf, is_async);
}

static void handle_elaphure_client(int client_sock) {
    uint8_t *rx_buf = heap_caps_malloc(EL_TCP_RX_BUF_SIZE, MALLOC_CAP_8BIT);
    uint8_t *tx_buf = heap_caps_malloc(EL_TCP_TX_BUF_SIZE, MALLOC_CAP_8BIT);
    if (!rx_buf || !tx_buf) {
        ESP_LOGE(TAG, "Malloc failed");
        goto cleanup;
    }

    int nodelay = 1;
    setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    // 1. 接收 12 字节握手包
    el_handshake_pkt_t hs_req;
    if (recv_exact(client_sock, (uint8_t *)&hs_req, sizeof(hs_req)) != sizeof(hs_req)) {
        ESP_LOGW(TAG, "Handshake recv failed");
        goto cleanup;
    }

    if (ntohl(hs_req.el_link_identifier) != EL_LINK_IDENTIFIER ||
        ntohl(hs_req.command) != EL_COMMAND_HANDSHAKE) {
        ESP_LOGW(TAG, "Invalid handshake header");
        goto cleanup;
    }

    uint32_t client_ver = ntohl(hs_req.version);
    uint32_t resp_ver = (client_ver >= EL_DAP_VERSION_ASYNC) ? EL_DAP_VERSION_ASYNC : client_ver;

    ESP_LOGI(TAG, "elaphureLink handshake OK (Client ver: 0x%" PRIx32 ", Negotiated: 0x%" PRIx32 ")",
             client_ver, resp_ver);

    // 2. 回复 12 字节握手包
    el_handshake_pkt_t hs_res = {
        .el_link_identifier = htonl(EL_LINK_IDENTIFIER),
        .command            = htonl(EL_COMMAND_HANDSHAKE),
        .version            = htonl(resp_ver),
    };
    if (send_exact(client_sock, (uint8_t *)&hs_res, sizeof(hs_res)) != sizeof(hs_res)) {
        goto cleanup;
    }

    bool is_async = false;

    // 3. 数据交互主循环（已移除频繁的 RX/TX Hex 日志打印以保证满速吞吐）
    while (s_is_running) {
        int r = recv(client_sock, rx_buf, EL_TCP_RX_BUF_SIZE, 0);
        if (r <= 0) {
            ESP_LOGI(TAG, "Client disconnected (r=%d, errno=%d)", r, errno);
            break;
        }

        if (rx_buf[0] == EL_VENDOR_COMMAND_PREFIX) {
            if (process_vendor_stream(client_sock, rx_buf, r, tx_buf, &is_async) < 0) break;
        } else {
            if (process_single_el_packet(client_sock, rx_buf, tx_buf, &is_async) < 0) break;
        }

        while (is_async && s_is_running) {
            if (recv_exact(client_sock, rx_buf, 4) <= 0) goto cleanup;
            uint16_t payload_len = ((uint16_t)rx_buf[2] << 8) | rx_buf[3];
            if (4 + payload_len > EL_TCP_RX_BUF_SIZE) goto cleanup;
            if (payload_len > 0) {
                if (recv_exact(client_sock, rx_buf + 4, payload_len) <= 0) goto cleanup;
            }
            if (process_single_el_packet(client_sock, rx_buf, tx_buf, &is_async) < 0) goto cleanup;
        }
    }

cleanup:
    if (rx_buf) free(rx_buf);
    if (tx_buf) free(tx_buf);
    shutdown(client_sock, SHUT_RDWR);
    close(client_sock);
}

static void elaphure_tcp_server_task(void *pvParameters) {
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(ELAPHURE_LINK_PORT),
    };

    s_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s_listen_sock < 0) {
        s_is_running = false;
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(s_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(s_listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0 ||
        listen(s_listen_sock, 1) != 0) {
        ESP_LOGE(TAG, "Bind/Listen failed on port %d: errno %d", ELAPHURE_LINK_PORT, errno);
        close(s_listen_sock);
        s_listen_sock = -1;
        s_is_running = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "elaphureLink TCP Server listening on port %d (DAP_PACKET_SIZE=%d)...",
             ELAPHURE_LINK_PORT, DAP_PACKET_SIZE);

    while (s_is_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(s_listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (client_sock < 0) break;

        handle_elaphure_client(client_sock);
    }

    if (s_listen_sock >= 0) {
        close(s_listen_sock);
        s_listen_sock = -1;
    }
    s_elp_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t wifi_dap_tcp_start(void) {
    if (s_is_running) return ESP_ERR_INVALID_STATE;

    DAP_Setup();

    s_is_running = true;
    if (xTaskCreatePinnedToCore(elaphure_tcp_server_task, "elp_tcp_srv", 4096, NULL, 6, &s_elp_task_handle, 1) != pdPASS) {
        s_is_running = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

void wifi_dap_tcp_stop(void) {
    if (!s_is_running) return;
    s_is_running = false;
    if (s_listen_sock >= 0) {
        shutdown(s_listen_sock, SHUT_RDWR);
        close(s_listen_sock);
        s_listen_sock = -1;
    }
}