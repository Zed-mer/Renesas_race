/*
 * Copyright (c) 2006-2023, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2021-10-10      Sherman      first version
 */

#ifndef __BOARD_H__
#define __BOARD_H__

#include "hal_data.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RA_SRAM_START   (0x22000000UL)
#define RA_SRAM_END     (0x220E2000UL) /* End of the CPU0 Solution RAM partition. */
#define RA_SRAM_SIZE    ((RA_SRAM_END - RA_SRAM_START) / 1024UL)

#ifdef __ARMCC_VERSION
extern int Image$$RAM_END$$ZI$$Base;
#define HEAP_BEGIN  ((void *)&Image$$RAM_END$$ZI$$Base)
#elif __ICCARM__
#pragma section="ram_BLOCK"
#define HEAP_BEGIN      (__segment_end("ram_BLOCK"))
#else
extern int __RAM_segment_used_end__;
#define HEAP_BEGIN      (&__RAM_segment_used_end__)
#endif

#define HEAP_END        RA_SRAM_END

#ifdef __cplusplus
}
#endif

#endif
