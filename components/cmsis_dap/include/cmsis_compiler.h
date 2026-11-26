#ifndef __CMSIS_COMPILER_H__
#define __CMSIS_COMPILER_H__

#include <stdint.h>

#ifndef __ASM
#define __ASM                   __asm
#endif
#ifndef __INLINE
#define __INLINE                inline
#endif
#ifndef __STATIC_INLINE
#define __STATIC_INLINE         static inline
#endif
#ifndef __STATIC_FORCEINLINE
#define __STATIC_FORCEINLINE    static inline __attribute__((always_inline))
#endif
#ifndef __NO_RETURN
#define __NO_RETURN             __attribute__((__noreturn__))
#endif
#ifndef __USED
#define __USED                  __attribute__((used))
#endif
#ifndef __WEAK
#define __WEAK                  __attribute__((weak))
#endif
#ifndef __PACKED
#define __PACKED                __attribute__((packed, aligned(1)))
#endif
#ifndef __PACKED_STRUCT
#define __PACKED_STRUCT         struct __attribute__((packed, aligned(1)))
#endif
#ifndef __PACKED_UNION
#define __PACKED_UNION          union __attribute__((packed, aligned(1)))
#endif

#ifndef __UNALIGNED_UINT32_WRITE
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpacked"
#pragma GCC diagnostic ignored "-Wattributes"
__PACKED_STRUCT T_UINT32_WRITE { uint32_t v; };
#pragma GCC diagnostic pop
#define __UNALIGNED_UINT32_WRITE(addr, val)    (void)((((struct T_UINT32_WRITE *)(void *)(addr))->v) = (val))
#endif

#ifndef __UNALIGNED_UINT32_READ
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpacked"
#pragma GCC diagnostic ignored "-Wattributes"
__PACKED_STRUCT T_UINT32_READ { uint32_t v; };
#pragma GCC diagnostic pop
#define __UNALIGNED_UINT32_READ(addr)          (((const struct T_UINT32_READ *)(const void *)(addr))->v)
#endif

#ifndef __UNALIGNED_UINT16_WRITE
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpacked"
#pragma GCC diagnostic ignored "-Wattributes"
__PACKED_STRUCT T_UINT16_WRITE { uint16_t v; };
#pragma GCC diagnostic pop
#define __UNALIGNED_UINT16_WRITE(addr, val)    (void)((((struct T_UINT16_WRITE *)(void *)(addr))->v) = (val))
#endif

#ifndef __UNALIGNED_UINT16_READ
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpacked"
#pragma GCC diagnostic ignored "-Wattributes"
__PACKED_STRUCT T_UINT16_READ { uint16_t v; };
#pragma GCC diagnostic pop
#define __UNALIGNED_UINT16_READ(addr)          (((const struct T_UINT16_READ *)(const void *)(addr))->v)
#endif

#endif /* __CMSIS_COMPILER_H__ */
