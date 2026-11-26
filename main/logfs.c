#include "logfs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "LOGFS";

#define LOGFS_BASE_PATH     "/logfs"
#define LOG_FILE_PREFIX     "uart_"
#define LOG_FILE_EXT        ".log"
#define LOG_MAX_FILE_SIZE   (1024 * 1024)
#define LOG_MAX_FILE_COUNT  5
#define LOG_MAX_INDEX       99999
#define LOG_SCAN_BUF_SIZE   32

static FILE *s_log_file = NULL;
static size_t s_current_file_size = 0;
static int s_current_index = 0;
static char s_current_path[64] = {0};
static SemaphoreHandle_t s_log_mutex = NULL;

static int get_sorted_log_indices(int *indices, int max_items) {
    DIR *dir = opendir(LOGFS_BASE_PATH);
    if (!dir) {
        ESP_LOGW(TAG, "[SCAN] opendir(%s) 失败, errno=%d (%s)", LOGFS_BASE_PATH, errno, strerror(errno));
        return 0;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max_items) {
        int idx = 0;
        char extra = 0;
        if (sscanf(entry->d_name, "uart_%d.log%c", &idx, &extra) == 1 &&
            idx >= 1 && idx <= LOG_MAX_INDEX) {
            indices[count++] = idx;
        }
    }
    closedir(dir);

    if (count <= 1) {
        return count;
    }

    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (indices[i] > indices[j]) {
                int tmp = indices[i];
                indices[i] = indices[j];
                indices[j] = tmp;
            }
        }
    }

    int split_pos = 0;
    for (int i = 0; i < count - 1; i++) {
        if (indices[i + 1] - indices[i] > (LOG_MAX_INDEX / 2)) {
            split_pos = i + 1;
            break;
        }
    }

    if (split_pos > 0) {
        int temp[LOG_SCAN_BUF_SIZE];
        int n = 0;
        for (int i = split_pos; i < count; i++) temp[n++] = indices[i];
        for (int i = 0; i < split_pos; i++) temp[n++] = indices[i];
        memcpy(indices, temp, count * sizeof(int));
    }

    return count;
}

static void cleanup_old_logs(void) {
    int indices[LOG_SCAN_BUF_SIZE];
    int count = get_sorted_log_indices(indices, LOG_SCAN_BUF_SIZE);

    int i = 0;
    while (count - i >= LOG_MAX_FILE_COUNT) {
        char old_path[64];
        snprintf(old_path, sizeof(old_path), "%s/%s%05d%s",
                 LOGFS_BASE_PATH, LOG_FILE_PREFIX, indices[i], LOG_FILE_EXT);
        if (unlink(old_path) == 0) {
            ESP_LOGI(TAG, "[CLEAN] 删除最早文件: %s", old_path);
        } else {
            ESP_LOGE(TAG, "[CLEAN] 删除旧文件失败: %s, errno=%d", old_path, errno);
        }
        i++;
    }
}

static esp_err_t rotate_to_next_file(void) {
    if (s_log_file) {
        fflush(s_log_file);
        fsync(fileno(s_log_file));
        fclose(s_log_file);
        s_log_file = NULL;
    }

    cleanup_old_logs();

    s_current_index = (s_current_index >= LOG_MAX_INDEX) ? 1 : (s_current_index + 1);
    snprintf(s_current_path, sizeof(s_current_path), "%s/%s%05d%s",
             LOGFS_BASE_PATH, LOG_FILE_PREFIX, s_current_index, LOG_FILE_EXT);

    s_log_file = fopen(s_current_path, "w");
    if (!s_log_file) {
        ESP_LOGE(TAG, "[ROTATE] 创建新文件失败: %s, errno=%d (%s)", s_current_path, errno, strerror(errno));
        return ESP_FAIL;
    }

    setvbuf(s_log_file, NULL, _IOFBF, 4096);
    s_current_file_size = 0;
    ESP_LOGI(TAG, "[ROTATE] 新建并打开文件: %s (fp=%p)", s_current_path, (void *)s_log_file);
    return ESP_OK;
}

esp_err_t logfs_init(void) {
    ESP_LOGI(TAG, "[INIT] 开始初始化 logfs...");

    if (s_log_mutex == NULL) {
        s_log_mutex = xSemaphoreCreateMutex();
        if (s_log_mutex == NULL) {
            ESP_LOGE(TAG, "[INIT] 创建互斥锁失败");
            return ESP_ERR_NO_MEM;
        }
    }

    esp_vfs_littlefs_conf_t conf = {
        .base_path = LOGFS_BASE_PATH,
        .partition_label = "logfs",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[INIT] LittleFS 挂载失败 (%s)", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    esp_littlefs_info(conf.partition_label, &total, &used);
    ESP_LOGI(TAG, "[INIT] 挂载成功: 总空间 %d KB, 已用 %d KB", (int)(total / 1024), (int)(used / 1024));

    int indices[LOG_SCAN_BUF_SIZE];
    int count = get_sorted_log_indices(indices, LOG_SCAN_BUF_SIZE);
    ESP_LOGI(TAG, "[INIT] 扫描到现有日志文件数: %d", count);

    if (count == 0) {
        s_current_index = 0;
        return rotate_to_next_file();
    }

    if (count > LOG_MAX_FILE_COUNT) {
        for (int i = 0; i < count - LOG_MAX_FILE_COUNT; i++) {
            char old_path[64];
            snprintf(old_path, sizeof(old_path), "%s/%s%05d%s",
                     LOGFS_BASE_PATH, LOG_FILE_PREFIX, indices[i], LOG_FILE_EXT);
            unlink(old_path);
        }
    }

    s_current_index = indices[count - 1];
    snprintf(s_current_path, sizeof(s_current_path), "%s/%s%05d%s",
             LOGFS_BASE_PATH, LOG_FILE_PREFIX, s_current_index, LOG_FILE_EXT);

    struct stat st;
    if (stat(s_current_path, &st) == 0 && st.st_size < LOG_MAX_FILE_SIZE) {
        s_log_file = fopen(s_current_path, "a");
        if (!s_log_file) {
            ESP_LOGE(TAG, "[INIT] 追加打开失败: %s, errno=%d (%s)", s_current_path, errno, strerror(errno));
            return ESP_FAIL;
        }
        setvbuf(s_log_file, NULL, _IOFBF, 4096);
        s_current_file_size = st.st_size;
        ESP_LOGI(TAG, "[INIT] 追加打开现有文件: %s (当前大小=%d B, fp=%p)",
                 s_current_path, (int)s_current_file_size, (void *)s_log_file);
        return ESP_OK;
    } else {
        return rotate_to_next_file();
    }
}

void logfs_write(const uint8_t *data, size_t len) {
    // 1. 入口状态检查
    if (!s_log_file) {
        ESP_LOGE(TAG, "[WRITE] 丢弃 %u B: s_log_file 为 NULL（未初始化或打开失败）！", (unsigned int)len);
        return;
    }
    if (!data || len == 0) {
        ESP_LOGW(TAG, "[WRITE] 无效参数: data=%p, len=%u", (void *)data, (unsigned int)len);
        return;
    }

    // 2. 打印收到的前几个字节（Hex + 文本预览）
    ESP_LOGI(TAG, "[WRITE] 准备写入 %u B -> 首字节: %02X %02X %02X",
             (unsigned int)len,
             len > 0 ? data[0] : 0,
             len > 1 ? data[1] : 0,
             len > 2 ? data[2] : 0);

    if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        size_t offset = 0;
        while (offset < len) {
            if (s_current_file_size >= LOG_MAX_FILE_SIZE) {
                if (rotate_to_next_file() != ESP_OK) {
                    break;
                }
            }

            size_t space_left = LOG_MAX_FILE_SIZE - s_current_file_size;
            size_t chunk = (len - offset < space_left) ? (len - offset) : space_left;

            size_t written = fwrite(data + offset, 1, chunk, s_log_file);
            s_current_file_size += written;
            offset += written;

            ESP_LOGI(TAG, "[WRITE] fwrite 完成: 请求=%u B, 实际=%u B, 内存累计=%u B, ferror=%d",
                     (unsigned int)chunk, (unsigned int)written,
                     (unsigned int)s_current_file_size, ferror(s_log_file));

            if (written < chunk) {
                ESP_LOGE(TAG, "[WRITE] 写入异常, errno=%d (%s)", errno, strerror(errno));
                break;
            }
        }
        xSemaphoreGive(s_log_mutex);
    } else {
        ESP_LOGE(TAG, "[WRITE] 获取 s_log_mutex 超时！丢弃 %u B", (unsigned int)len);
    }
}

void logfs_flush(void) {
    if (!s_log_file) {
        ESP_LOGW(TAG, "[FLUSH] 跳过: s_log_file 为 NULL");
        return;
    }

    if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        int r1 = fflush(s_log_file);
        int r2 = fsync(fileno(s_log_file)); // 强制同步 LittleFS 元数据（确保 stat 文件大小立即更新）

        struct stat st = {0};
        stat(s_current_path, &st);
        ESP_LOGI(TAG, "[FLUSH] fflush=%d, fsync=%d, 磁盘实际大小=%ld B (%s)",
                 r1, r2, (long)st.st_size, s_current_path);

        xSemaphoreGive(s_log_mutex);
    } else {
        ESP_LOGE(TAG, "[FLUSH] 获取 s_log_mutex 超时！");
    }
}