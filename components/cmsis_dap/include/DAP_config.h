#ifndef __DAP_CONFIG_H__
#define __DAP_CONFIG_H__

#include <stdint.h>
#include <stddef.h>
#include "driver/gpio.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

// ============================================================================
// 1. ESP32-S3 核心参数与 CMSIS-DAP 协议配置
// ============================================================================
#define CPU_CLOCK               240000000U  // ESP32-S3 主频 240MHz
#define IO_PORT_WRITE_CYCLES    1U          // 寄存器直写周期（直写寄存器可降至 1U）
#define DAP_SWD                 1           // 启用 SWD 模式
#define DAP_JTAG                0           // 关闭 JTAG（精简编译体积）
#define DAP_JTAG_DEV_CNT        8U
#define DAP_DEFAULT_PORT        1U          // 默认调试端口: 1 = SWD
#define DAP_DEFAULT_SWJ_CLOCK   1000000U    // 默认 SWD 时钟: 1MHz
#define DAP_PACKET_SIZE         64U         // 与 WinUSB Bulk 端点 wMaxPacketSize (64) 一致
#define DAP_PACKET_COUNT        1U          // 管道包缓冲数量
#define SWO_UART                0
#define SWO_MANCHESTER          0
#define SWO_BUFFER_SIZE         512U
#define SWO_STREAM              0
#define DAP_UART                0
#define DAP_UART_USB_COM_PORT   0
#define DAP_UART_RX_BUFFER_SIZE 512U
#define DAP_UART_TX_BUFFER_SIZE 512U
#define TIMESTAMP_CLOCK         1000000U    // 1MHz 时间戳

#define DAP_CONFIG_ROM_VENDOR   "Espressif"
#define DAP_CONFIG_PRODUCT_STR  "ESP32-S3 CMSIS-DAP Composite"
#define DAP_CONFIG_SER_NUM_STR  "123456"
#define DAP_CONFIG_FW_VER_STR   "2.1.2"

// ============================================================================
// 2. ESP32-S3 GPIO 引脚分配与编译期常量掩码（均位于 Bank0 低 32 位寄存器）
// ============================================================================
#define PIN_SWCLK_GPIO          GPIO_NUM_1
#define PIN_SWDIO_GPIO          GPIO_NUM_2
#define PIN_NRESET_GPIO         GPIO_NUM_3
#define PIN_LED_CONN_GPIO       GPIO_NUM_15

#define SWCLK_MASK              (1UL << PIN_SWCLK_GPIO)
#define SWDIO_MASK              (1UL << PIN_SWDIO_GPIO)
#define NRESET_MASK             (1UL << PIN_NRESET_GPIO)
#define LED_CONN_MASK           (1UL << PIN_LED_CONN_GPIO)

// 编译器内联宏兼容
#ifndef __STATIC_INLINE
#define __STATIC_INLINE         static inline
#endif
#ifndef __STATIC_FORCEINLINE
#define __STATIC_FORCEINLINE    static inline __attribute__((always_inline))
#endif
#ifndef __WEAK
#define __WEAK                  __attribute__((weak))
#endif

// 高精度微秒时间戳获取
__STATIC_FORCEINLINE uint32_t TIMESTAMP_GET(void) {
    return (uint32_t)esp_timer_get_time();
}

// ============================================================================
// 3. 硬件端口初始化与释放（已修复 nRESET 在 Connect under Reset 下的控制时序）
// ============================================================================
__STATIC_INLINE void PORT_SWD_SETUP(void) {
    // SWCLK 配置为纯推挽输出
    gpio_config_t clk_conf = {
        .pin_bit_mask = (1ULL << PIN_SWCLK_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&clk_conf);

    // SWDIO 配置为 INPUT_OUTPUT（一次性使能输入缓冲，后续切换方向仅需写 enable_w1ts/w1tc）
    gpio_config_t dio_conf = {
        .pin_bit_mask = (1ULL << PIN_SWDIO_GPIO),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&dio_conf);

    GPIO.out_w1tc = SWCLK_MASK;
    GPIO.out_w1ts = SWDIO_MASK;

    // 确保 nRESET 处于开漏输出使能状态且默认高电平释放
    GPIO.out_w1ts = NRESET_MASK;
    GPIO.enable_w1ts = NRESET_MASK;
}

__STATIC_INLINE void PORT_JTAG_SETUP(void) {
    // 未启用 JTAG
}

__STATIC_INLINE void PORT_OFF(void) {
    // 仅释放 SWCLK 和 SWDIO，保留 NRESET 的输出使能，确保未连 SWD 前也能正常拉低复位脚
    GPIO.enable_w1tc = (SWCLK_MASK | SWDIO_MASK);
    GPIO.out_w1ts = NRESET_MASK;
}

// ============================================================================
// 4. 寄存器直写级 GPIO 读写（零函数调用开销，原子单周期写外设总线）
// ============================================================================
__STATIC_FORCEINLINE uint32_t PIN_SWCLK_TCK_IN(void) {
    return (GPIO.in >> PIN_SWCLK_GPIO) & 1U;
}
__STATIC_FORCEINLINE void PIN_SWCLK_TCK_SET(void) {
    GPIO.out_w1ts = SWCLK_MASK;
}
__STATIC_FORCEINLINE void PIN_SWCLK_TCK_CLR(void) {
    GPIO.out_w1tc = SWCLK_MASK;
}

__STATIC_FORCEINLINE uint32_t PIN_SWDIO_TMS_IN(void) {
    return (GPIO.in >> PIN_SWDIO_GPIO) & 1U;
}
__STATIC_FORCEINLINE void PIN_SWDIO_TMS_SET(void) {
    GPIO.out_w1ts = SWDIO_MASK;
}
__STATIC_FORCEINLINE void PIN_SWDIO_TMS_CLR(void) {
    GPIO.out_w1tc = SWDIO_MASK;
}

__STATIC_FORCEINLINE uint32_t PIN_SWDIO_IN(void) {
    return (GPIO.in >> PIN_SWDIO_GPIO) & 1U;
}
__STATIC_FORCEINLINE void PIN_SWDIO_OUT(uint32_t bit) {
    if (bit & 1U) {
        GPIO.out_w1ts = SWDIO_MASK;
    } else {
        GPIO.out_w1tc = SWDIO_MASK;
    }
}
__STATIC_FORCEINLINE void PIN_SWDIO_OUT_ENABLE(void) {
    GPIO.enable_w1ts = SWDIO_MASK;
}
__STATIC_FORCEINLINE void PIN_SWDIO_OUT_DISABLE(void) {
    GPIO.enable_w1tc = SWDIO_MASK;
}

// JTAG 桩函数（供 DAP.c 链接通过）
__STATIC_FORCEINLINE uint32_t PIN_TDI_IN(void)            { return 0U; }
__STATIC_FORCEINLINE void     PIN_TDI_OUT(uint32_t bit)   { (void)bit; }
__STATIC_FORCEINLINE uint32_t PIN_TDO_IN(void)            { return 0U; }
__STATIC_FORCEINLINE uint32_t PIN_nTRST_IN(void)          { return 1U; }
__STATIC_FORCEINLINE void     PIN_nTRST_OUT(uint32_t bit) { (void)bit; }

// nRESET 复位脚操作（强制确保输出使能有效，兼容 Strapping 引脚特性）
__STATIC_FORCEINLINE uint32_t PIN_nRESET_IN(void) {
    return (GPIO.in >> PIN_NRESET_GPIO) & 1U;
}
__STATIC_FORCEINLINE void PIN_nRESET_OUT(uint32_t bit) {
    GPIO.enable_w1ts = NRESET_MASK;
    if (bit & 1U) {
        GPIO.out_w1ts = NRESET_MASK;
    } else {
        GPIO.out_w1tc = NRESET_MASK;
    }
}

// ============================================================================
// 5. LED 指示灯、复位与微秒级延时
// ============================================================================
__STATIC_INLINE void LED_CONNECTED_OUT(uint32_t bit) {
    if (bit & 1U) {
        GPIO.out_w1ts = LED_CONN_MASK;
    } else {
        GPIO.out_w1tc = LED_CONN_MASK;
    }
}
__STATIC_INLINE void LED_RUNNING_OUT(uint32_t bit) {
    (void)bit;
}

__STATIC_INLINE void DAP_SETUP(void) {
    gpio_reset_pin(PIN_NRESET_GPIO);
    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << PIN_LED_CONN_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_conf);
    GPIO.out_w1tc = LED_CONN_MASK;

    // 上电初始化即配置 GPIO3 为开漏输出并置高释放，保证任何时候 DAP_SWJ_Pins 都能拉低 RSTB
    gpio_config_t rst_conf = {
        .pin_bit_mask = (1ULL << PIN_NRESET_GPIO),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst_conf);
    GPIO.out_w1ts = NRESET_MASK;
    GPIO.enable_w1ts = NRESET_MASK;

    PORT_OFF();
}

__STATIC_INLINE uint32_t RESET_TARGET(void) {
    // 主动输出 20ms 低电平硬件复位脉冲，兼容直接调用 DAP_ResetTarget 的上位机
    PIN_nRESET_OUT(0U);
    esp_rom_delay_us(20000U);
    PIN_nRESET_OUT(1U);
    esp_rom_delay_us(5000U);
    return 1U;
}

__STATIC_FORCEINLINE void PIN_DELAY_SLOW(uint32_t delay) {
    uint32_t count = delay;
    while (count--) { __asm__ __volatile__("nop"); }
}

__STATIC_FORCEINLINE void PIN_DELAY_FAST(void) {
    __asm__ __volatile__("nop");
}

__STATIC_FORCEINLINE void PIN_DELAY(void) {
    __asm__ __volatile__("nop");
}

__STATIC_INLINE void DELAY_MS(uint32_t ms) {
    esp_rom_delay_us(ms * 1000U);
}

__STATIC_INLINE void DELAY_US(uint32_t us) {
    esp_rom_delay_us(us);
}

// ============================================================================
// 6. DAP_Info 信息查询回调函数
// ============================================================================
__STATIC_INLINE uint8_t DAP_GetVendorString(char *str) {
    const char *s = DAP_CONFIG_ROM_VENDOR;
    uint8_t len = 0;
    while (s[len]) { str[len] = s[len]; len++; }
    str[len] = '\0';
    return len + 1;
}

__STATIC_INLINE uint8_t DAP_GetProductString(char *str) {
    const char *s = DAP_CONFIG_PRODUCT_STR;
    uint8_t len = 0;
    while (s[len]) { str[len] = s[len]; len++; }
    str[len] = '\0';
    return len + 1;
}

__STATIC_INLINE uint8_t DAP_GetSerNumString(char *str) {
    const char *s = DAP_CONFIG_SER_NUM_STR;
    uint8_t len = 0;
    while (s[len]) { str[len] = s[len]; len++; }
    str[len] = '\0';
    return len + 1;
}

__STATIC_INLINE uint8_t DAP_GetTargetDeviceVendorString(char *str) { (void)str; return 0; }
__STATIC_INLINE uint8_t DAP_GetTargetDeviceNameString(char *str)   { (void)str; return 0; }
__STATIC_INLINE uint8_t DAP_GetTargetBoardVendorString(char *str)  { (void)str; return 0; }
__STATIC_INLINE uint8_t DAP_GetTargetBoardNameString(char *str)    { (void)str; return 0; }

__STATIC_INLINE uint8_t DAP_GetProductFirmwareVersionString(char *str) {
    const char *s = DAP_CONFIG_FW_VER_STR;
    uint8_t len = 0;
    while (s[len]) { str[len] = s[len]; len++; }
    str[len] = '\0';
    return len + 1;
}

#endif /* __DAP_CONFIG_H__ */