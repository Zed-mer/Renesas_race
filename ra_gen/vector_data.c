/* generated vector source file - do not edit */
#include "bsp_api.h"
/* Do not build these data structures if no interrupts are currently allocated because IAR will have build errors. */
#if VECTOR_DATA_IRQ_COUNT > 0
        BSP_DONT_REMOVE const fsp_vector_t g_vector_table[BSP_ICU_VECTOR_MAX_ENTRIES] BSP_PLACE_IN_SECTION(BSP_SECTION_APPLICATION_VECTORS) =
        {
                        [0] = sci_uart_rxi_isr, /* SCI7 RXI (Received data full) */
            [1] = sci_uart_txi_isr, /* SCI7 TXI (Transmit data empty) */
            [2] = sci_uart_tei_isr, /* SCI7 TEI (Transmit end) */
            [3] = sci_uart_eri_isr, /* SCI7 ERI (Receive error) */
            [4] = spi_rxi_isr, /* SPI0 RXI (Receive buffer full) */
            [5] = spi_txi_isr, /* SPI0 TXI (Transmit buffer empty) */
            [6] = spi_tei_isr, /* SPI0 TEI (Transmission complete event) */
            [7] = spi_eri_isr, /* SPI0 ERI (Error) */
            [8] = r_icu_isr, /* ICU IRQ8 (External pin interrupt 8) */
            [9] = r_icu_isr, /* ICU IRQ6 (External pin interrupt 6) */
            [10] = sci_spi_rxi_isr, /* SCI2 RXI (Received data full) */
            [11] = sci_spi_txi_isr, /* SCI2 TXI (Transmit data empty) */
            [12] = sci_spi_tei_isr, /* SCI2 TEI (Transmit end) */
            [13] = sci_spi_eri_isr, /* SCI2 ERI (Receive error) */
            [14] = r_icu_isr, /* ICU IRQ9 (External pin interrupt 9) */
            [15] = gpt_counter_overflow_isr, /* GPT2 COUNTER OVERFLOW (Overflow) */
            [16] = gpt_counter_overflow_isr, /* GPT3 COUNTER OVERFLOW (Overflow) */
        };
        const bsp_interrupt_event_t g_interrupt_event_link_select[BSP_ICU_VECTOR_MAX_ENTRIES] =
        {
            [0] = BSP_PRV_IELS_ENUM(EVENT_SCI7_RXI), /* SCI7 RXI (Received data full) */
            [1] = BSP_PRV_IELS_ENUM(EVENT_SCI7_TXI), /* SCI7 TXI (Transmit data empty) */
            [2] = BSP_PRV_IELS_ENUM(EVENT_SCI7_TEI), /* SCI7 TEI (Transmit end) */
            [3] = BSP_PRV_IELS_ENUM(EVENT_SCI7_ERI), /* SCI7 ERI (Receive error) */
            [4] = BSP_PRV_IELS_ENUM(EVENT_SPI0_RXI), /* SPI0 RXI (Receive buffer full) */
            [5] = BSP_PRV_IELS_ENUM(EVENT_SPI0_TXI), /* SPI0 TXI (Transmit buffer empty) */
            [6] = BSP_PRV_IELS_ENUM(EVENT_SPI0_TEI), /* SPI0 TEI (Transmission complete event) */
            [7] = BSP_PRV_IELS_ENUM(EVENT_SPI0_ERI), /* SPI0 ERI (Error) */
            [8] = BSP_PRV_IELS_ENUM(EVENT_ICU_IRQ8), /* ICU IRQ8 (External pin interrupt 8) */
            [9] = BSP_PRV_IELS_ENUM(EVENT_ICU_IRQ6), /* ICU IRQ6 (External pin interrupt 6) */
            [10] = BSP_PRV_IELS_ENUM(EVENT_SCI2_RXI), /* SCI2 RXI (Received data full) */
            [11] = BSP_PRV_IELS_ENUM(EVENT_SCI2_TXI), /* SCI2 TXI (Transmit data empty) */
            [12] = BSP_PRV_IELS_ENUM(EVENT_SCI2_TEI), /* SCI2 TEI (Transmit end) */
            [13] = BSP_PRV_IELS_ENUM(EVENT_SCI2_ERI), /* SCI2 ERI (Receive error) */
            [14] = BSP_PRV_IELS_ENUM(EVENT_ICU_IRQ9), /* ICU IRQ9 (External pin interrupt 9) */
            [15] = BSP_PRV_IELS_ENUM(EVENT_GPT2_COUNTER_OVERFLOW), /* GPT2 COUNTER OVERFLOW (Overflow) */
            [16] = BSP_PRV_IELS_ENUM(EVENT_GPT3_COUNTER_OVERFLOW), /* GPT3 COUNTER OVERFLOW (Overflow) */
        };
        #endif
