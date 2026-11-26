#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "DAP_config.h"
#include "web_server.h"
#include "logfs.h"

static const char *TAG = "WEB_UART";

/* 复用 main.c 中已初始化的 UART_NUM_1 */
#define TARGET_UART_NUM UART_NUM_1
#define OTA_BUF_SIZE    4096
#define LOGFS_DIR_PATH  "/logfs"

static httpd_handle_t s_server = NULL;
static int s_ws_fd = -1;

/* ================= 嵌入的离线 Gzip 静态资源 ================= */
extern const uint8_t xterm_css_gz_start[] asm("_binary_xterm_css_gz_start");
extern const uint8_t xterm_css_gz_end[]   asm("_binary_xterm_css_gz_end");

extern const uint8_t xterm_js_gz_start[]  asm("_binary_xterm_js_gz_start");
extern const uint8_t xterm_js_gz_end[]    asm("_binary_xterm_js_gz_end");

extern const uint8_t fit_js_gz_start[]    asm("_binary_xterm_addon_fit_js_gz_start");
extern const uint8_t fit_js_gz_end[]      asm("_binary_xterm_addon_fit_js_gz_end");

/* ================= 内嵌 xterm.js 网页终端（含日志下载弹窗） ================= */
static const char INDEX_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>\n"
"<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
"<title>ESP32-S3 无线串口终端</title>\n"
"<link rel='stylesheet' href='/xterm.css'/>\n"
"<script src='/xterm.js'></script>\n"
"<script src='/xterm-addon-fit.js'></script>\n"
"<style>\n"
"*{box-sizing:border-box;margin:0;padding:0}\n"
"body{position:fixed;inset:0;background:#1e1e1e;color:#ccc;font-family:sans-serif;display:grid;grid-template-rows:auto minmax(0,1fr) auto;overflow:hidden}\n"
".bar{background:#2d2d2d;padding:8px 14px;display:flex;align-items:center;gap:10px;border-bottom:1px solid #444;flex-wrap:wrap;z-index:10}\n"
".dot{width:10px;height:10px;border-radius:50%;background:#f5222d}\n"
".dot.on{background:#52c41a}\n"
"select,button,input[type=text]{background:#3c3c3c;color:#fff;border:1px solid #555;padding:6px 10px;border-radius:4px;font-size:13px;outline:none}\n"
"button{cursor:pointer}\n"
"button:hover{background:#4a4a4a}\n"
"button.pri{background:#1677ff;border-color:#1677ff}\n"
"button.pri:hover{background:#4096ff}\n"
"button.warn{background:#d4380d;border-color:#d4380d}\n"
"button.suc{background:#389e0d;border-color:#389e0d}\n"
"button.suc:hover{background:#52c41a}\n"
"#term{width:100%;height:100%;padding:6px;overflow:hidden}\n"
".send-bar{background:#252526;padding:8px 14px;display:flex;align-items:center;gap:8px;border-top:1px solid #444;z-index:10}\n"
".send-bar input[type=text]{flex:1;min-width:0;font-family:monospace;background:#1e1e1e;border-color:#555}\n"
".send-bar input[type=text]:focus{border-color:#1677ff}\n"
".chk{display:flex;align-items:center;gap:4px;font-size:13px;cursor:pointer;user-select:none;white-space:nowrap}\n"
".modal{display:none;position:fixed;inset:0;background:rgba(0,0,0,.65);z-index:100;align-items:center;justify-content:center}\n"
".modal.show{display:flex}\n"
".modal-box{background:#252526;border:1px solid #444;border-radius:6px;width:90%;max-width:440px;padding:16px;box-shadow:0 8px 24px rgba(0,0,0,.5)}\n"
".modal-hd{display:flex;justify-content:space-between;align-items:center;margin-bottom:12px;padding-bottom:8px;border-bottom:1px solid #3c3c3c;font-weight:bold;color:#fff}\n"
".log-item{display:flex;justify-content:space-between;align-items:center;padding:8px 10px;background:#1e1e1e;border:1px solid #383838;border-radius:4px;margin-bottom:8px;font-family:monospace;font-size:13px}\n"
"</style></head><body>\n"
"<div class='bar'>\n"
"<span id='dot' class='dot'></span>\n"
"<span id='st'>未连接</span>\n"
"<select id='baud' onchange='setBaud()'>\n"
"<option value='9600'>9600</option>\n"
"<option value='115200' selected>115200</option>\n"
"<option value='230400'>230400</option>\n"
"<option value='460800'>460800</option>\n"
"<option value='921600'>921600</option>\n"
"</select>\n"
"<button class='pri' onclick='connectWS()'>重连</button>\n"
"<button onclick='term.clear()'>清屏</button>\n"
"<button class='warn' onclick='resetTarget()'>复位目标板</button>\n"
"<button class='suc' onclick='openLogModal()'>下载日志</button>\n"
"<input type='file' id='otaFile' accept='.bin' style='display:none' onchange='uploadOta(this)'>\n"
"<button id='otaBtn' onclick=\"document.getElementById('otaFile').click()\">固件升级</button>\n"
"<span id='otaMsg' style='font-size:12px;color:#52c41a'></span>\n"
"</div>\n"
"<div id='term'></div>\n"
"<div class='send-bar'>\n"
"<input type='text' id='txInput' placeholder='输入要发送的串口指令，按 Enter 发送...' onkeydown='if(event.key===\"Enter\")sendText()'>\n"
"<select id='eol' title='行尾结束符'>\n"
"<option value='crlf' selected>\\r\\n (CRLF)</option>\n"
"<option value='lf'>\\n (LF)</option>\n"
"<option value='cr'>\\r (CR)</option>\n"
"<option value='none'>无</option>\n"
"</select>\n"
"<label class='chk'><input type='checkbox' id='echo' checked>本地回显</label>\n"
"<button class='pri' onclick='sendText()'>发送</button>\n"
"</div>\n"
"<div id='logModal' class='modal' onclick='if(event.target===this)closeLogModal()'>\n"
"<div class='modal-box'>\n"
"<div class='modal-hd'><span>串口日志文件 (/logfs)</span><button onclick='closeLogModal()' style='padding:2px 8px'>✕</button></div>\n"
"<div id='logList' style='max-height:280px;overflow-y:auto'>加载中...</div>\n"
"<div style='margin-top:12px;display:flex;justify-content:space-between;align-items:center'>\n"
"<button onclick='openLogModal()'>刷新列表</button>\n"
"<a href='/api/log_dl?all=1' download='uart_all_logs.log'><button class='pri'>一键合并下载全部</button></a>\n"
"</div></div></div>\n"
"<script>\n"
"window.onerror=function(msg){document.getElementById('st').innerText='JS错误: '+msg;};\n"
"const term=new Terminal({cursorBlink:true,fontSize:14,convertEol:true,theme:{background:'#1e1e1e'}});\n"
"const fitAddon=new FitAddon.FitAddon();\n"
"term.loadAddon(fitAddon);\n"
"const termEl=document.getElementById('term');\n"
"term.open(termEl);\n"
"function doFit(){\n"
"  const h=window.innerHeight-document.querySelector('.bar').offsetHeight-document.querySelector('.send-bar').offsetHeight;\n"
"  termEl.style.height=Math.max(100,h)+'px';\n"
"  fitAddon.fit();\n"
"}\n"
"window.addEventListener('resize',doFit);\n"
"setTimeout(doFit,50);\n"
"let ws=null;\n"
"function connectWS(){\n"
"  if(ws)ws.close();\n"
"  ws=new WebSocket('ws://'+location.host+'/ws');\n"
"  ws.binaryType='arraybuffer';\n"
"  ws.onopen=()=>{document.getElementById('dot').classList.add('on');document.getElementById('st').innerText='已连接';term.writeln('\\r\\n\\x1b[32m[ESP32-S3] WebSocket 串口透传已就绪\\x1b[0m');doFit();};\n"
"  ws.onmessage=(e)=>{term.write(new Uint8Array(e.data));};\n"
"  ws.onclose=()=>{document.getElementById('dot').classList.remove('on');document.getElementById('st').innerText='已断开';};\n"
"}\n"
"term.onData(data=>{if(ws&&ws.readyState===1)ws.send(data);});\n"
"function sendText(){\n"
"  const inp=document.getElementById('txInput');\n"
"  const eolType=document.getElementById('eol').value;\n"
"  const eolMap={crlf:'\\r\\n',lf:'\\n',cr:'\\r',none:''};\n"
"  const eol=eolMap[eolType]||'';\n"
"  const echo=document.getElementById('echo').checked;\n"
"  if(!ws||ws.readyState!==1){term.writeln('\\r\\n\\x1b[31m[错误] WebSocket 未连接\\x1b[0m');return;}\n"
"  const payload=inp.value+eol;\n"
"  if(payload.length===0)return;\n"
"  ws.send(payload);\n"
"  if(echo)term.writeln('\\x1b[36m> '+inp.value+'\\x1b[0m');\n"
"  inp.value='';\n"
"}\n"
"async function setBaud(){\n"
"  let b=document.getElementById('baud').value;\n"
"  let r=await fetch('/api/baud?val='+b,{method:'POST'});\n"
"  term.writeln('\\r\\n\\x1b[33m[ESP32-S3] 波特率切换为: '+await r.text()+'\\x1b[0m');\n"
"}\n"
"async function resetTarget(){\n"
"  await fetch('/api/reset',{method:'POST'});\n"
"  term.writeln('\\r\\n\\x1b[31m[ESP32-S3] 已发送 nRESET 复位脉冲\\x1b[0m');\n"
"}\n"
"function fmtSize(b){if(b<1024)return b+' B';if(b<1048576)return (b/1024).toFixed(1)+' KB';return (b/1048576).toFixed(2)+' MB';}\n"
"async function openLogModal(){\n"
"  document.getElementById('logModal').classList.add('show');\n"
"  const box=document.getElementById('logList');\n"
"  box.innerHTML='正在刷新并读取目录...';\n"
"  try{\n"
"    const r=await fetch('/api/logs');\n"
"    const list=await r.json();\n"
"    if(!list.length){box.innerHTML='<div style=\"color:#888;padding:12px 0\">暂无日志文件</div>';return;}\n"
"    box.innerHTML=list.map(f=>`<div class='log-item'><span>${f.name} <span style='color:#888'>(${fmtSize(f.size)})</span></span><a href='/api/log_dl?file=${encodeURIComponent(f.name)}' download='${f.name}'><button class='suc' style='padding:4px 10px'>下载</button></a></div>`).join('');\n"
"  }catch(e){box.innerHTML='<div style=\"color:#f5222d\">读取失败: '+e+'</div>';}\n"
"}\n"
"function closeLogModal(){document.getElementById('logModal').classList.remove('show');}\n"
"function uploadOta(input){\n"
"  const file=input.files[0];\n"
"  if(!file)return;\n"
"  const msg=document.getElementById('otaMsg');\n"
"  const btn=document.getElementById('otaBtn');\n"
"  btn.disabled=true;\n"
"  msg.style.color='#52c41a';\n"
"  term.writeln('\\r\\n\\x1b[33m[OTA] 开始上传固件: '+file.name+' ('+file.size+' B)\\x1b[0m');\n"
"  const xhr=new XMLHttpRequest();\n"
"  xhr.open('POST','/api/ota',true);\n"
"  xhr.upload.onprogress=function(e){\n"
"    if(e.lengthComputable){\n"
"      const pct=Math.round((e.loaded/e.total)*100);\n"
"      msg.innerText='升级中: '+pct+'%';\n"
"    }\n"
"  };\n"
"  xhr.onload=function(){\n"
"    if(xhr.status===200){\n"
"      msg.innerText='升级成功，正在重启...';\n"
"      term.writeln('\\r\\n\\x1b[32m[OTA] 固件校验通过，ESP32-S3 正在重启...\\x1b[0m');\n"
"      setTimeout(()=>location.reload(),4000);\n"
"    }else{\n"
"      btn.disabled=false;\n"
"      msg.style.color='#f5222d';\n"
"      msg.innerText='失败: '+xhr.responseText;\n"
"      term.writeln('\\r\\n\\x1b[31m[OTA] 升级失败: '+xhr.responseText+'\\x1b[0m');\n"
"    }\n"
"  };\n"
"  xhr.onerror=function(){\n"
"    btn.disabled=false;\n"
"    msg.style.color='#f5222d';\n"
"    msg.innerText='网络异常';\n"
"    term.writeln('\\r\\n\\x1b[31m[OTA] 上传过程中网络中断\\x1b[0m');\n"
"  };\n"
"  xhr.send(file);\n"
"  input.value='';\n"
"}\n"
"connectWS();\n"
"</script></body></html>";

/* ================= 线程安全的 WebSocket 异步推送 ================= */
typedef struct {
    uint8_t *data;
    size_t len;
} ws_send_arg_t;

static void ws_async_send_cb(void *arg)
{
    ws_send_arg_t *pkt = (ws_send_arg_t *)arg;
    if (s_server != NULL && s_ws_fd >= 0) {
        httpd_ws_frame_t ws_pkt = {
            .final = true,
            .fragmented = false,
            .type = HTTPD_WS_TYPE_BINARY,
            .payload = pkt->data,
            .len = pkt->len
        };
        if (httpd_ws_send_frame_async(s_server, s_ws_fd, &ws_pkt) != ESP_OK) {
            s_ws_fd = -1;
        }
    }
    free(pkt->data);
    free(pkt);
}

void web_server_send_uart_data(const uint8_t *data, size_t len)
{
    if (s_server == NULL || s_ws_fd < 0 || data == NULL || len == 0) {
        return;
    }
    ws_send_arg_t *pkt = malloc(sizeof(ws_send_arg_t));
    if (!pkt) return;

    pkt->data = malloc(len);
    if (!pkt->data) {
        free(pkt);
        return;
    }
    memcpy(pkt->data, data, len);
    pkt->len = len;

    if (httpd_queue_work(s_server, ws_async_send_cb, pkt) != ESP_OK) {
        free(pkt->data);
        free(pkt);
    }
}

/* ================= 静态资源与 HTTP / WebSocket 路由回调 ================= */
static esp_err_t send_gz_asset(httpd_req_t *req, const char *name, const char *content_type, const uint8_t *start, const uint8_t *end)
{
    size_t len = end - start;
    ESP_LOGI(TAG, "HTTP GET %s -> size=%u B, magic=%02X %02X %02X",
             name, (unsigned int)len,
             len > 0 ? start[0] : 0,
             len > 1 ? start[1] : 0,
             len > 2 ? start[2] : 0);

    if (len < 100) {
        ESP_LOGE(TAG, "ERROR: %s is empty or corrupted (%u B)!", name, (unsigned int)len);
    }

    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    return httpd_resp_send(req, (const char *)start, len);
}

static esp_err_t xterm_css_handler(httpd_req_t *req)
{
    return send_gz_asset(req, "/xterm.css", "text/css", xterm_css_gz_start, xterm_css_gz_end);
}

static esp_err_t xterm_js_handler(httpd_req_t *req)
{
    return send_gz_asset(req, "/xterm.js", "application/javascript", xterm_js_gz_start, xterm_js_gz_end);
}

static esp_err_t fit_js_handler(httpd_req_t *req)
{
    return send_gz_asset(req, "/xterm-addon-fit.js", "application/javascript", fit_js_gz_start, fit_js_gz_end);
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "HTTP GET / (INDEX_HTML len=%u B)", (unsigned int)strlen(INDEX_HTML));
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        s_ws_fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "WebSocket client connected, fd=%d", s_ws_fd);
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK || ws_pkt.len == 0) {
        return ret;
    }

    uint8_t *buf = malloc(ws_pkt.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    ws_pkt.payload = buf;

    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret == ESP_OK) {
        uart_write_bytes(TARGET_UART_NUM, (const char *)ws_pkt.payload, ws_pkt.len);
    }
    free(buf);
    return ret;
}

static esp_err_t baud_post_handler(httpd_req_t *req)
{
    char query[32] = {0};
    char val_str[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "val", val_str, sizeof(val_str)) == ESP_OK) {
        uint32_t baud = (uint32_t)atoi(val_str);
        if (baud >= 1200 && baud <= 2000000) {
            uart_set_baudrate(TARGET_UART_NUM, baud);
            ESP_LOGI(TAG, "Web changed UART1 baudrate to %" PRIu32, baud);
            return httpd_resp_sendstr(req, val_str);
        }
    }
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, "Invalid baudrate");
}

static esp_err_t reset_post_handler(httpd_req_t *req)
{
    gpio_set_direction(PIN_NRESET_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_NRESET_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_NRESET_GPIO, 1);
    return httpd_resp_sendstr(req, "OK");
}

/* ================= 日志列表与下载接口 ================= */
static int sort_log_indices_helper(int *indices, int max_items)
{
    DIR *dir = opendir(LOGFS_DIR_PATH);
    if (!dir) return 0;

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max_items) {
        int idx = 0;
        char extra = 0;
        if (sscanf(entry->d_name, "uart_%d.log%c", &idx, &extra) == 1 && idx >= 1 && idx <= 99999) {
            indices[count++] = idx;
        }
    }
    closedir(dir);

    if (count <= 1) return count;

    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (indices[i] > indices[j]) {
                int t = indices[i];
                indices[i] = indices[j];
                indices[j] = t;
            }
        }
    }

    int split = 0;
    for (int i = 0; i < count - 1; i++) {
        if (indices[i + 1] - indices[i] > 50000) {
            split = i + 1;
            break;
        }
    }
    if (split > 0) {
        int tmp[16];
        int n = 0;
        for (int i = split; i < count; i++) tmp[n++] = indices[i];
        for (int i = 0; i < split; i++) tmp[n++] = indices[i];
        memcpy(indices, tmp, count * sizeof(int));
    }
    return count;
}

// GET /api/logs -> 返回 JSON 文件列表 [{"name":"uart_00001.log","size":123}, ...]
static esp_err_t logs_list_get_handler(httpd_req_t *req)
{
    logfs_flush(); // 先把缓冲区数据刷入 Flash，保证大小实时准确

    int indices[16];
    int count = sort_log_indices_helper(indices, 16);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req, "[");

    char item[96];
    char path[64];
    bool first = true;
    for (int i = 0; i < count; i++) {
        snprintf(path, sizeof(path), "%s/uart_%05d.log", LOGFS_DIR_PATH, indices[i]);
        struct stat st;
        if (stat(path, &st) == 0) {
            snprintf(item, sizeof(item), "%s{\"name\":\"uart_%05d.log\",\"size\":%ld}",
                     first ? "" : ",", indices[i], (long)st.st_size);
            httpd_resp_sendstr_chunk(req, item);
            first = false;
        }
    }
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

// 流式发送单个文件数据块（不发送结束空包，便于多文件合并）
static esp_err_t stream_file_chunks(httpd_req_t *req, const char *filepath)
{
    FILE *f = fopen(filepath, "rb");
    if (!f) return ESP_FAIL;

    char *buf = malloc(2048);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    size_t n;
    while ((n = fread(buf, 1, 2048, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
    }
    free(buf);
    fclose(f);
    return err;
}

// GET /api/log_dl?file=uart_00001.log  或  /api/log_dl?all=1
static esp_err_t log_download_get_handler(httpd_req_t *req)
{
    logfs_flush(); // 下载前强制刷盘，确保最后几字节完整导出

    char query[64] = {0};
    char param[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
        return ESP_FAIL;
    }

    // 1. 一键合并下载全部日志
    if (httpd_query_key_value(query, "all", param, sizeof(param)) == ESP_OK && strcmp(param, "1") == 0) {
        int indices[16];
        int count = sort_log_indices_helper(indices, 16);
        if (count == 0) {
            httpd_resp_send_404(req);
            return ESP_FAIL;
        }

        httpd_resp_set_type(req, "application/octet-stream");
        httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"uart_all_logs.log\"");

        char path[64];
        for (int i = 0; i < count; i++) {
            snprintf(path, sizeof(path), "%s/uart_%05d.log", LOGFS_DIR_PATH, indices[i]);
            if (stream_file_chunks(req, path) != ESP_OK) {
                httpd_resp_send_chunk(req, NULL, 0);
                return ESP_FAIL;
            }
        }
        return httpd_resp_send_chunk(req, NULL, 0);
    }

    // 2. 下载指定单个文件
    if (httpd_query_key_value(query, "file", param, sizeof(param)) == ESP_OK) {
        // 严格防路径穿越校验：只允许匹配 uart_xxxxx.log
        int idx = 0;
        char extra = 0;
        if (sscanf(param, "uart_%d.log%c", &idx, &extra) != 1 || idx < 1 || idx > 99999) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
            return ESP_FAIL;
        }

        char filepath[64];
        char disp[80];
        snprintf(filepath, sizeof(filepath), "%s/uart_%05d.log", LOGFS_DIR_PATH, idx);
        snprintf(disp, sizeof(disp), "attachment; filename=\"uart_%05d.log\"", idx);

        struct stat st;
        if (stat(filepath, &st) != 0) {
            httpd_resp_send_404(req);
            return ESP_FAIL;
        }

        httpd_resp_set_type(req, "application/octet-stream");
        httpd_resp_set_hdr(req, "Content-Disposition", disp);

        esp_err_t err = stream_file_chunks(req, filepath);
        httpd_resp_send_chunk(req, NULL, 0);
        return err;
    }

    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid parameters");
    return ESP_FAIL;
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    if (req->content_len <= 0 || req->content_len > update_partition->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid firmware size");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Starting OTA -> partition: %s, offset: 0x%" PRIx32 ", size: %d B",
             update_partition->label, update_partition->address, req->content_len);

    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char *buf = malloc(OTA_BUF_SIZE);
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, (remaining < OTA_BUF_SIZE) ? remaining : OTA_BUF_SIZE);
        if (recv_len <= 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "OTA receive failed: %d", recv_len);
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }

        err = esp_ota_write(ota_handle, buf, recv_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Flash write failed");
            return ESP_FAIL;
        }
        remaining -= recv_len;
    }
    free(buf);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware validation failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA success! Rebooting in 1s...");
    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

/* ================= 启动与停止服务器 ================= */
httpd_handle_t start_web_server(void)
{
    if (s_server != NULL) {
        return s_server;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 14; // 增加了 2 个日志接口，确保上限充足

    if (httpd_start(&s_server, &config) == ESP_OK) {
        ESP_LOGI(TAG, "Embedded assets check: css=%u B, js=%u B, fit=%u B",
                 (unsigned int)(xterm_css_gz_end - xterm_css_gz_start),
                 (unsigned int)(xterm_js_gz_end - xterm_js_gz_start),
                 (unsigned int)(fit_js_gz_end - fit_js_gz_start));

        httpd_uri_t uri_index   = { .uri = "/",                    .method = HTTP_GET,  .handler = index_get_handler };
        httpd_uri_t uri_css     = { .uri = "/xterm.css",           .method = HTTP_GET,  .handler = xterm_css_handler };
        httpd_uri_t uri_js      = { .uri = "/xterm.js",            .method = HTTP_GET,  .handler = xterm_js_handler };
        httpd_uri_t uri_fit_js  = { .uri = "/xterm-addon-fit.js",  .method = HTTP_GET,  .handler = fit_js_handler };
        httpd_uri_t uri_baud    = { .uri = "/api/baud",            .method = HTTP_POST, .handler = baud_post_handler };
        httpd_uri_t uri_reset   = { .uri = "/api/reset",           .method = HTTP_POST, .handler = reset_post_handler };
        httpd_uri_t uri_ota     = { .uri = "/api/ota",             .method = HTTP_POST, .handler = ota_post_handler };
        httpd_uri_t uri_logs    = { .uri = "/api/logs",            .method = HTTP_GET,  .handler = logs_list_get_handler };
        httpd_uri_t uri_log_dl  = { .uri = "/api/log_dl",          .method = HTTP_GET,  .handler = log_download_get_handler };
        httpd_uri_t uri_ws      = { .uri = "/ws",                  .method = HTTP_GET,  .handler = ws_handler, .is_websocket = true };

        httpd_register_uri_handler(s_server, &uri_index);
        httpd_register_uri_handler(s_server, &uri_css);
        httpd_register_uri_handler(s_server, &uri_js);
        httpd_register_uri_handler(s_server, &uri_fit_js);
        httpd_register_uri_handler(s_server, &uri_baud);
        httpd_register_uri_handler(s_server, &uri_reset);
        httpd_register_uri_handler(s_server, &uri_ota);
        httpd_register_uri_handler(s_server, &uri_logs);
        httpd_register_uri_handler(s_server, &uri_log_dl);
        httpd_register_uri_handler(s_server, &uri_ws);
        ESP_LOGI(TAG, "HTTP/WS xterm.js server (offline) started on port 80");
    }
    return s_server;
}

void stop_web_server(httpd_handle_t server)
{
    if (server != NULL) {
        httpd_stop(server);
        s_server = NULL;
        s_ws_fd = -1;
    }
}