/* generated HAL source file - do not edit */
#include "hal_data.h"
sci_b_uart_instance_ctrl_t     g_uart8_ctrl;

            sci_b_baud_setting_t               g_uart8_baud_setting =
            {
                /* Baud rate calculated with 0.469% error. */ .baudrate_bits_b.abcse = 0, .baudrate_bits_b.abcs = 0, .baudrate_bits_b.bgdm = 1, .baudrate_bits_b.cks = 0, .baudrate_bits_b.brr = 53, .baudrate_bits_b.mddr = (uint8_t) 256, .baudrate_bits_b.brme = false
            };

            /** UART extended configuration for UARTonSCI HAL driver */
            const sci_b_uart_extended_cfg_t g_uart8_cfg_extend =
            {
                .clock                = SCI_B_UART_CLOCK_INT,
                .rx_edge_start          = SCI_B_UART_START_BIT_FALLING_EDGE,
                .noise_cancel         = SCI_B_UART_NOISE_CANCELLATION_DISABLE,
                .rx_fifo_trigger        = SCI_B_UART_RX_FIFO_TRIGGER_MAX,
                .p_baud_setting         = &g_uart8_baud_setting,
                .flow_control           = SCI_B_UART_FLOW_CONTROL_RTS,
                #if 0xFF != 0xFF
                .flow_control_pin       = BSP_IO_PORT_FF_PIN_0xFF,
                #else
                .flow_control_pin       = (bsp_io_port_pin_t) UINT16_MAX,
                #endif
                .rs485_setting          = {
                    .enable = SCI_B_UART_RS485_DISABLE,
                    .polarity = SCI_B_UART_RS485_DE_POLARITY_HIGH,
                    .assertion_time = 1,
                    .negation_time = 1,
                },
                .delay_cycles = 0,
            };

            /** UART interface configuration */
            const uart_cfg_t g_uart8_cfg =
            {
                .channel             = 8,
                .data_bits           = UART_DATA_BITS_8,
                .parity              = UART_PARITY_OFF,
                .stop_bits           = UART_STOP_BITS_1,
                .p_callback          = user_uart8_callback,
                .p_context           = NULL,
                .p_extend            = &g_uart8_cfg_extend,
#define RA_NOT_DEFINED (1)
#if (RA_NOT_DEFINED == RA_NOT_DEFINED)
                .p_transfer_tx       = NULL,
#else
                .p_transfer_tx       = &RA_NOT_DEFINED,
#endif
#if (RA_NOT_DEFINED == RA_NOT_DEFINED)
                .p_transfer_rx       = NULL,
#else
                .p_transfer_rx       = &RA_NOT_DEFINED,
#endif
#undef RA_NOT_DEFINED
                .rxi_ipl             = (12),
                .txi_ipl             = (12),
                .tei_ipl             = (12),
                .eri_ipl             = (12),
#if defined(VECTOR_NUMBER_SCI8_RXI)
                .rxi_irq             = VECTOR_NUMBER_SCI8_RXI,
#else
                .rxi_irq             = FSP_INVALID_VECTOR,
#endif
#if defined(VECTOR_NUMBER_SCI8_TXI)
                .txi_irq             = VECTOR_NUMBER_SCI8_TXI,
#else
                .txi_irq             = FSP_INVALID_VECTOR,
#endif
#if defined(VECTOR_NUMBER_SCI8_TEI)
                .tei_irq             = VECTOR_NUMBER_SCI8_TEI,
#else
                .tei_irq             = FSP_INVALID_VECTOR,
#endif
#if defined(VECTOR_NUMBER_SCI8_ERI)
                .eri_irq             = VECTOR_NUMBER_SCI8_ERI,
#else
                .eri_irq             = FSP_INVALID_VECTOR,
#endif
            };

/* Instance structure to use this module. */
const uart_instance_t g_uart8 =
{
    .p_ctrl        = &g_uart8_ctrl,
    .p_cfg         = &g_uart8_cfg,
    .p_api         = &g_uart_on_sci_b
};

ospi_b_instance_ctrl_t g_ospi1_ctrl;

static ospi_b_timing_setting_t g_ospi1_timing_settings =
{
    .command_to_command_interval = OSPI_B_COMMAND_INTERVAL_CLOCKS_8,
    .cs_pullup_lag               = OSPI_B_COMMAND_CS_PULLUP_CLOCKS_NO_EXTENSION,
    .cs_pulldown_lead            = OSPI_B_COMMAND_CS_PULLDOWN_CLOCKS_NO_EXTENSION,
    .sdr_drive_timing            = OSPI_B_SDR_DRIVE_TIMING_BEFORE_CK,
    .sdr_sampling_edge           = OSPI_B_CK_EDGE_FALLING,
    .sdr_sampling_delay          = OSPI_B_SDR_SAMPLING_DELAY_NONE,
    .ddr_sampling_extension      = OSPI_B_DDR_SAMPLING_EXTENSION_1,
};
extern ospi_b_xspi_command_set_t g_hyper_ram_commands[];
extern spi_flash_erase_command_t g_hyper_ram_erase_commands[];
static const ospi_b_table_t g_ospi1_command_set =
{
    .p_table = (void *) g_hyper_ram_commands,
    .length = 1
};

#if OSPI_B_CFG_DOTF_SUPPORT_ENABLE
extern uint8_t g_ospi_dotf_iv[];
extern uint8_t g_ospi_dotf_key[];

static ospi_b_dotf_cfg_t g_ospi_dotf_cfg=
{
    .key_type       = OSPI_B_DOTF_AES_KEY_TYPE_128,
    .format         = OSPI_B_DOTF_KEY_FORMAT_PLAINTEXT,
    .p_start_addr   = (uint32_t *)0x80000000,
    .p_end_addr     = (uint32_t *)0x80001FFF,
    .p_key          = (uint32_t *)g_ospi_dotf_key,
    .p_iv           = (uint32_t *)g_ospi_dotf_iv,
};
#endif

static const ospi_b_extended_cfg_t g_ospi1_extended_cfg =
{
    .ospi_b_unit                             = 1,
    .channel                                 = (ospi_b_device_number_t) 0,
    .p_timing_settings                       = &g_ospi1_timing_settings,
    .p_xspi_command_set                      = &g_ospi1_command_set,
    .data_latch_delay_clocks                 = OSPI_B_DS_TIMING_DELAY_8,
    .p_autocalibration_preamble_pattern_addr = (uint8_t *) 0x00,
#if OSPI_B_CFG_DMAC_SUPPORT_ENABLE
    .p_lower_lvl_transfer                    = &RA_NOT_DEFINED,
#endif
#if OSPI_B_CFG_DOTF_SUPPORT_ENABLE
    .p_dotf_cfg                              = &g_ospi_dotf_cfg,
#endif
#if OSPI_B_CFG_ROW_ADDRESSING_SUPPORT_ENABLE
    .row_index_bytes                         = 0xFF,
#endif
};
const spi_flash_cfg_t g_ospi1_cfg =
{
    .spi_protocol                = SPI_FLASH_PROTOCOL_8D_8D_8D,
    .read_mode                   = SPI_FLASH_READ_MODE_STANDARD, /* Unused by OSPI_B */
    .address_bytes               = SPI_FLASH_ADDRESS_BYTES_4,
    .dummy_clocks                = SPI_FLASH_DUMMY_CLOCKS_DEFAULT, /* Unused by OSPI_B */
    .page_program_address_lines  = (spi_flash_data_lines_t) 0U, /* Unused by OSPI_B */
    .page_size_bytes             = 64,
    .write_status_bit            = 1,
    .write_enable_bit            = 1,
    .page_program_command        = 0, /* OSPI_B uses command sets. See g_ospi1_command_set. */
    .write_enable_command        = 0, /* OSPI_B uses command sets. See g_ospi1_command_set. */
    .status_command              = 0, /* OSPI_B uses command sets. See g_ospi1_command_set. */
    .read_command                = 0, /* OSPI_B uses command sets. See g_ospi1_command_set. */
#if OSPI_B_CFG_XIP_SUPPORT_ENABLE
    .xip_enter_command           = 0,
    .xip_exit_command            = 0,
#else
    .xip_enter_command           = 0U,
    .xip_exit_command            = 0U,
#endif
    /* OSPI_B uses command sets, this is kept for backwards compatibility. See g_ospi1_command_set. */
    .erase_command_list_length   = 1,
    .p_erase_command_list        = (spi_flash_erase_command_t const *) g_hyper_ram_erase_commands,
    .p_extend                    = &g_ospi1_extended_cfg,
};

/** This structure encompasses everything that is needed to use an instance of this interface. */
const spi_flash_instance_t g_ospi1 =
{
    .p_ctrl = &g_ospi1_ctrl,
    .p_cfg =  &g_ospi1_cfg,
    .p_api =  &g_ospi_b_on_spi_flash,
};

#if defined OSPI_B_CFG_DOTF_PROTECTED_MODE_SUPPORT_ENABLE
rsip_instance_t const * const gp_rsip_instance = &RA_NOT_DEFINED;
#endif
rmac_instance_ctrl_t g_ether0_ctrl;
            static rmac_buffer_node_t g_ether0_buffer_node_list[193];

            uint8_t g_ether0_mac_address[6] = { 0x00,0x11,0x22,0x33,0x44,0x55 };

            layer3_switch_ts_reception_process_descriptor_t g_ether0_ts_descriptor_array0[8];rmac_queue_info_t g_ether0_ts_queue[1] =
 {
{ .queue_cfg={.array_length          = 8,
.p_descriptor_array    = NULL,
.p_ts_descriptor_array = g_ether0_ts_descriptor_array0,
.ports                 = (1 << 0),
.type                  = LAYER3_SWITCH_QUEUE_TYPE_TX,
.write_back_mode       = LAYER3_SWITCH_WRITE_BACK_MODE_FULL,
.descriptor_format     = LAYER3_SWITCH_DISCRIPTOR_FORMTAT_TX_TIMESTAMP,
.rx_timestamp_storage  = LAYER3_SWITCH_RX_TIMESTAMP_STORAGE_DISABLE,
}},
};
            layer3_switch_descriptor_t           g_ether0_tx_descriptor_array0[15+1];layer3_switch_descriptor_t           g_ether0_tx_descriptor_array1[15+1];rmac_queue_info_t g_ether0_tx_queue_list[2] =
 {
{ .queue_cfg={.array_length       = 15+1,
.p_descriptor_array = g_ether0_tx_descriptor_array0,
.p_ts_descriptor_array = NULL,
.ports              = (1 << 0 ),
.type               = LAYER3_SWITCH_QUEUE_TYPE_TX,
.write_back_mode    = LAYER3_SWITCH_WRITE_BACK_MODE_FULL,
.descriptor_format  = LAYER3_SWITCH_DISCRIPTOR_FORMTAT_EXTENDED,
.rx_timestamp_storage = LAYER3_SWITCH_RX_TIMESTAMP_STORAGE_DISABLE,
}},
{ .queue_cfg={.array_length       = 15+1,
.p_descriptor_array = g_ether0_tx_descriptor_array1,
.p_ts_descriptor_array = NULL,
.ports              = (1 << 0 ),
.type               = LAYER3_SWITCH_QUEUE_TYPE_TX,
.write_back_mode    = LAYER3_SWITCH_WRITE_BACK_MODE_FULL,
.descriptor_format  = LAYER3_SWITCH_DISCRIPTOR_FORMTAT_EXTENDED,
.rx_timestamp_storage = LAYER3_SWITCH_RX_TIMESTAMP_STORAGE_DISABLE,
}},
};
            layer3_switch_descriptor_t           g_ether0_rx_descriptor_array0[63+1];layer3_switch_descriptor_t           g_ether0_rx_descriptor_array1[63+1];rmac_queue_info_t g_ether0_rx_queue_list[2] =
 {
{ .queue_cfg={.array_length       = 63+1,
.p_descriptor_array = g_ether0_rx_descriptor_array0,
.p_ts_descriptor_array = NULL,
.ports              = (1 << 0) | (0x0),
.type               = LAYER3_SWITCH_QUEUE_TYPE_RX,
.write_back_mode    = LAYER3_SWITCH_WRITE_BACK_MODE_FULL,
.descriptor_format  = LAYER3_SWITCH_DISCRIPTOR_FORMTAT_EXTENDED,
#if LAYER3_SWITCH_CFG_GPTP_ENABLE
.rx_timestamp_storage = LAYER3_SWITCH_RX_TIMESTAMP_STORAGE_ENABLE,
#else
.rx_timestamp_storage = LAYER3_SWITCH_RX_TIMESTAMP_STORAGE_DISABLE,
#endif
}},
{ .queue_cfg={.array_length       = 63+1,
.p_descriptor_array = g_ether0_rx_descriptor_array1,
.p_ts_descriptor_array = NULL,
.ports              = (1 << 0) | (0x0),
.type               = LAYER3_SWITCH_QUEUE_TYPE_RX,
.write_back_mode    = LAYER3_SWITCH_WRITE_BACK_MODE_FULL,
.descriptor_format  = LAYER3_SWITCH_DISCRIPTOR_FORMTAT_EXTENDED,
#if LAYER3_SWITCH_CFG_GPTP_ENABLE
.rx_timestamp_storage = LAYER3_SWITCH_RX_TIMESTAMP_STORAGE_ENABLE,
#else
.rx_timestamp_storage = LAYER3_SWITCH_RX_TIMESTAMP_STORAGE_DISABLE,
#endif
}},
};

            const rmac_extended_cfg_t g_ether0_extended_cfg_t =
            {
                .p_ether_switch      = &g_layer3_switch0,
                .tx_queue_num        = 2,
                .rx_queue_num        = 2,

                .p_ts_queue     = g_ether0_ts_queue,
                .p_tx_queue_list     = g_ether0_tx_queue_list,
                .p_rx_queue_list     = g_ether0_rx_queue_list,
#if defined(VECTOR_NUMBER_ETHER_RMPI0)
                .rmpi_irq                = VECTOR_NUMBER_ETHER_RMPI0,
#else
                .rmpi_irq                = FSP_INVALID_VECTOR,
#endif
                .rmpi_ipl                = (BSP_IRQ_DISABLED),
                .p_buffer_node_list      = g_ether0_buffer_node_list,
                .buffer_node_num         = 193,
                .transmission_descriptor_format       = RMAC_TRANSMISSION_DESCRIPTOR_FORMAT_DIRECT,
            };
            uint8_t g_ether0_ether_buffer0[1536];
uint8_t g_ether0_ether_buffer1[1536];
uint8_t g_ether0_ether_buffer2[1536];
uint8_t g_ether0_ether_buffer3[1536];
uint8_t g_ether0_ether_buffer4[1536];
uint8_t g_ether0_ether_buffer5[1536];
uint8_t g_ether0_ether_buffer6[1536];
uint8_t g_ether0_ether_buffer7[1536];
uint8_t g_ether0_ether_buffer8[1536];
uint8_t g_ether0_ether_buffer9[1536];
uint8_t g_ether0_ether_buffer10[1536];
uint8_t g_ether0_ether_buffer11[1536];
uint8_t g_ether0_ether_buffer12[1536];
uint8_t g_ether0_ether_buffer13[1536];
uint8_t g_ether0_ether_buffer14[1536];
uint8_t g_ether0_ether_buffer15[1536];
uint8_t g_ether0_ether_buffer16[1536];
uint8_t g_ether0_ether_buffer17[1536];
uint8_t g_ether0_ether_buffer18[1536];
uint8_t g_ether0_ether_buffer19[1536];
uint8_t g_ether0_ether_buffer20[1536];
uint8_t g_ether0_ether_buffer21[1536];
uint8_t g_ether0_ether_buffer22[1536];
uint8_t g_ether0_ether_buffer23[1536];
uint8_t g_ether0_ether_buffer24[1536];
uint8_t g_ether0_ether_buffer25[1536];
uint8_t g_ether0_ether_buffer26[1536];
uint8_t g_ether0_ether_buffer27[1536];
uint8_t g_ether0_ether_buffer28[1536];
uint8_t g_ether0_ether_buffer29[1536];
uint8_t g_ether0_ether_buffer30[1536];
uint8_t g_ether0_ether_buffer31[1536];
uint8_t g_ether0_ether_buffer32[1536];
uint8_t g_ether0_ether_buffer33[1536];
uint8_t g_ether0_ether_buffer34[1536];
uint8_t g_ether0_ether_buffer35[1536];
uint8_t g_ether0_ether_buffer36[1536];
uint8_t g_ether0_ether_buffer37[1536];
uint8_t g_ether0_ether_buffer38[1536];
uint8_t g_ether0_ether_buffer39[1536];
uint8_t g_ether0_ether_buffer40[1536];
uint8_t g_ether0_ether_buffer41[1536];
uint8_t g_ether0_ether_buffer42[1536];
uint8_t g_ether0_ether_buffer43[1536];
uint8_t g_ether0_ether_buffer44[1536];
uint8_t g_ether0_ether_buffer45[1536];
uint8_t g_ether0_ether_buffer46[1536];
uint8_t g_ether0_ether_buffer47[1536];
uint8_t g_ether0_ether_buffer48[1536];
uint8_t g_ether0_ether_buffer49[1536];
uint8_t g_ether0_ether_buffer50[1536];
uint8_t g_ether0_ether_buffer51[1536];
uint8_t g_ether0_ether_buffer52[1536];
uint8_t g_ether0_ether_buffer53[1536];
uint8_t g_ether0_ether_buffer54[1536];
uint8_t g_ether0_ether_buffer55[1536];
uint8_t g_ether0_ether_buffer56[1536];
uint8_t g_ether0_ether_buffer57[1536];
uint8_t g_ether0_ether_buffer58[1536];
uint8_t g_ether0_ether_buffer59[1536];
uint8_t g_ether0_ether_buffer60[1536];
uint8_t g_ether0_ether_buffer61[1536];
uint8_t g_ether0_ether_buffer62[1536];
uint8_t g_ether0_ether_buffer63[1536];
uint8_t g_ether0_ether_buffer64[1536];
uint8_t g_ether0_ether_buffer65[1536];
uint8_t g_ether0_ether_buffer66[1536];
uint8_t g_ether0_ether_buffer67[1536];
uint8_t g_ether0_ether_buffer68[1536];
uint8_t g_ether0_ether_buffer69[1536];
uint8_t g_ether0_ether_buffer70[1536];
uint8_t g_ether0_ether_buffer71[1536];
uint8_t g_ether0_ether_buffer72[1536];
uint8_t g_ether0_ether_buffer73[1536];
uint8_t g_ether0_ether_buffer74[1536];
uint8_t g_ether0_ether_buffer75[1536];
uint8_t g_ether0_ether_buffer76[1536];
uint8_t g_ether0_ether_buffer77[1536];
uint8_t g_ether0_ether_buffer78[1536];
uint8_t g_ether0_ether_buffer79[1536];
uint8_t g_ether0_ether_buffer80[1536];
uint8_t g_ether0_ether_buffer81[1536];
uint8_t g_ether0_ether_buffer82[1536];
uint8_t g_ether0_ether_buffer83[1536];
uint8_t g_ether0_ether_buffer84[1536];
uint8_t g_ether0_ether_buffer85[1536];
uint8_t g_ether0_ether_buffer86[1536];
uint8_t g_ether0_ether_buffer87[1536];
uint8_t g_ether0_ether_buffer88[1536];
uint8_t g_ether0_ether_buffer89[1536];
uint8_t g_ether0_ether_buffer90[1536];
uint8_t g_ether0_ether_buffer91[1536];
uint8_t g_ether0_ether_buffer92[1536];
uint8_t g_ether0_ether_buffer93[1536];
uint8_t g_ether0_ether_buffer94[1536];
uint8_t g_ether0_ether_buffer95[1536];
uint8_t g_ether0_ether_buffer96[1536];
uint8_t g_ether0_ether_buffer97[1536];
uint8_t g_ether0_ether_buffer98[1536];
uint8_t g_ether0_ether_buffer99[1536];
uint8_t g_ether0_ether_buffer100[1536];
uint8_t g_ether0_ether_buffer101[1536];
uint8_t g_ether0_ether_buffer102[1536];
uint8_t g_ether0_ether_buffer103[1536];
uint8_t g_ether0_ether_buffer104[1536];
uint8_t g_ether0_ether_buffer105[1536];
uint8_t g_ether0_ether_buffer106[1536];
uint8_t g_ether0_ether_buffer107[1536];
uint8_t g_ether0_ether_buffer108[1536];
uint8_t g_ether0_ether_buffer109[1536];
uint8_t g_ether0_ether_buffer110[1536];
uint8_t g_ether0_ether_buffer111[1536];
uint8_t g_ether0_ether_buffer112[1536];
uint8_t g_ether0_ether_buffer113[1536];
uint8_t g_ether0_ether_buffer114[1536];
uint8_t g_ether0_ether_buffer115[1536];
uint8_t g_ether0_ether_buffer116[1536];
uint8_t g_ether0_ether_buffer117[1536];
uint8_t g_ether0_ether_buffer118[1536];
uint8_t g_ether0_ether_buffer119[1536];
uint8_t g_ether0_ether_buffer120[1536];
uint8_t g_ether0_ether_buffer121[1536];
uint8_t g_ether0_ether_buffer122[1536];
uint8_t g_ether0_ether_buffer123[1536];
uint8_t g_ether0_ether_buffer124[1536];
uint8_t g_ether0_ether_buffer125[1536];
uint8_t g_ether0_ether_buffer126[1536];
uint8_t g_ether0_ether_buffer127[1536];

            uint8_t *pp_g_ether0_ether_buffers[128] = {
(uint8_t *) &g_ether0_ether_buffer0[0],
(uint8_t *) &g_ether0_ether_buffer1[0],
(uint8_t *) &g_ether0_ether_buffer2[0],
(uint8_t *) &g_ether0_ether_buffer3[0],
(uint8_t *) &g_ether0_ether_buffer4[0],
(uint8_t *) &g_ether0_ether_buffer5[0],
(uint8_t *) &g_ether0_ether_buffer6[0],
(uint8_t *) &g_ether0_ether_buffer7[0],
(uint8_t *) &g_ether0_ether_buffer8[0],
(uint8_t *) &g_ether0_ether_buffer9[0],
(uint8_t *) &g_ether0_ether_buffer10[0],
(uint8_t *) &g_ether0_ether_buffer11[0],
(uint8_t *) &g_ether0_ether_buffer12[0],
(uint8_t *) &g_ether0_ether_buffer13[0],
(uint8_t *) &g_ether0_ether_buffer14[0],
(uint8_t *) &g_ether0_ether_buffer15[0],
(uint8_t *) &g_ether0_ether_buffer16[0],
(uint8_t *) &g_ether0_ether_buffer17[0],
(uint8_t *) &g_ether0_ether_buffer18[0],
(uint8_t *) &g_ether0_ether_buffer19[0],
(uint8_t *) &g_ether0_ether_buffer20[0],
(uint8_t *) &g_ether0_ether_buffer21[0],
(uint8_t *) &g_ether0_ether_buffer22[0],
(uint8_t *) &g_ether0_ether_buffer23[0],
(uint8_t *) &g_ether0_ether_buffer24[0],
(uint8_t *) &g_ether0_ether_buffer25[0],
(uint8_t *) &g_ether0_ether_buffer26[0],
(uint8_t *) &g_ether0_ether_buffer27[0],
(uint8_t *) &g_ether0_ether_buffer28[0],
(uint8_t *) &g_ether0_ether_buffer29[0],
(uint8_t *) &g_ether0_ether_buffer30[0],
(uint8_t *) &g_ether0_ether_buffer31[0],
(uint8_t *) &g_ether0_ether_buffer32[0],
(uint8_t *) &g_ether0_ether_buffer33[0],
(uint8_t *) &g_ether0_ether_buffer34[0],
(uint8_t *) &g_ether0_ether_buffer35[0],
(uint8_t *) &g_ether0_ether_buffer36[0],
(uint8_t *) &g_ether0_ether_buffer37[0],
(uint8_t *) &g_ether0_ether_buffer38[0],
(uint8_t *) &g_ether0_ether_buffer39[0],
(uint8_t *) &g_ether0_ether_buffer40[0],
(uint8_t *) &g_ether0_ether_buffer41[0],
(uint8_t *) &g_ether0_ether_buffer42[0],
(uint8_t *) &g_ether0_ether_buffer43[0],
(uint8_t *) &g_ether0_ether_buffer44[0],
(uint8_t *) &g_ether0_ether_buffer45[0],
(uint8_t *) &g_ether0_ether_buffer46[0],
(uint8_t *) &g_ether0_ether_buffer47[0],
(uint8_t *) &g_ether0_ether_buffer48[0],
(uint8_t *) &g_ether0_ether_buffer49[0],
(uint8_t *) &g_ether0_ether_buffer50[0],
(uint8_t *) &g_ether0_ether_buffer51[0],
(uint8_t *) &g_ether0_ether_buffer52[0],
(uint8_t *) &g_ether0_ether_buffer53[0],
(uint8_t *) &g_ether0_ether_buffer54[0],
(uint8_t *) &g_ether0_ether_buffer55[0],
(uint8_t *) &g_ether0_ether_buffer56[0],
(uint8_t *) &g_ether0_ether_buffer57[0],
(uint8_t *) &g_ether0_ether_buffer58[0],
(uint8_t *) &g_ether0_ether_buffer59[0],
(uint8_t *) &g_ether0_ether_buffer60[0],
(uint8_t *) &g_ether0_ether_buffer61[0],
(uint8_t *) &g_ether0_ether_buffer62[0],
(uint8_t *) &g_ether0_ether_buffer63[0],
(uint8_t *) &g_ether0_ether_buffer64[0],
(uint8_t *) &g_ether0_ether_buffer65[0],
(uint8_t *) &g_ether0_ether_buffer66[0],
(uint8_t *) &g_ether0_ether_buffer67[0],
(uint8_t *) &g_ether0_ether_buffer68[0],
(uint8_t *) &g_ether0_ether_buffer69[0],
(uint8_t *) &g_ether0_ether_buffer70[0],
(uint8_t *) &g_ether0_ether_buffer71[0],
(uint8_t *) &g_ether0_ether_buffer72[0],
(uint8_t *) &g_ether0_ether_buffer73[0],
(uint8_t *) &g_ether0_ether_buffer74[0],
(uint8_t *) &g_ether0_ether_buffer75[0],
(uint8_t *) &g_ether0_ether_buffer76[0],
(uint8_t *) &g_ether0_ether_buffer77[0],
(uint8_t *) &g_ether0_ether_buffer78[0],
(uint8_t *) &g_ether0_ether_buffer79[0],
(uint8_t *) &g_ether0_ether_buffer80[0],
(uint8_t *) &g_ether0_ether_buffer81[0],
(uint8_t *) &g_ether0_ether_buffer82[0],
(uint8_t *) &g_ether0_ether_buffer83[0],
(uint8_t *) &g_ether0_ether_buffer84[0],
(uint8_t *) &g_ether0_ether_buffer85[0],
(uint8_t *) &g_ether0_ether_buffer86[0],
(uint8_t *) &g_ether0_ether_buffer87[0],
(uint8_t *) &g_ether0_ether_buffer88[0],
(uint8_t *) &g_ether0_ether_buffer89[0],
(uint8_t *) &g_ether0_ether_buffer90[0],
(uint8_t *) &g_ether0_ether_buffer91[0],
(uint8_t *) &g_ether0_ether_buffer92[0],
(uint8_t *) &g_ether0_ether_buffer93[0],
(uint8_t *) &g_ether0_ether_buffer94[0],
(uint8_t *) &g_ether0_ether_buffer95[0],
(uint8_t *) &g_ether0_ether_buffer96[0],
(uint8_t *) &g_ether0_ether_buffer97[0],
(uint8_t *) &g_ether0_ether_buffer98[0],
(uint8_t *) &g_ether0_ether_buffer99[0],
(uint8_t *) &g_ether0_ether_buffer100[0],
(uint8_t *) &g_ether0_ether_buffer101[0],
(uint8_t *) &g_ether0_ether_buffer102[0],
(uint8_t *) &g_ether0_ether_buffer103[0],
(uint8_t *) &g_ether0_ether_buffer104[0],
(uint8_t *) &g_ether0_ether_buffer105[0],
(uint8_t *) &g_ether0_ether_buffer106[0],
(uint8_t *) &g_ether0_ether_buffer107[0],
(uint8_t *) &g_ether0_ether_buffer108[0],
(uint8_t *) &g_ether0_ether_buffer109[0],
(uint8_t *) &g_ether0_ether_buffer110[0],
(uint8_t *) &g_ether0_ether_buffer111[0],
(uint8_t *) &g_ether0_ether_buffer112[0],
(uint8_t *) &g_ether0_ether_buffer113[0],
(uint8_t *) &g_ether0_ether_buffer114[0],
(uint8_t *) &g_ether0_ether_buffer115[0],
(uint8_t *) &g_ether0_ether_buffer116[0],
(uint8_t *) &g_ether0_ether_buffer117[0],
(uint8_t *) &g_ether0_ether_buffer118[0],
(uint8_t *) &g_ether0_ether_buffer119[0],
(uint8_t *) &g_ether0_ether_buffer120[0],
(uint8_t *) &g_ether0_ether_buffer121[0],
(uint8_t *) &g_ether0_ether_buffer122[0],
(uint8_t *) &g_ether0_ether_buffer123[0],
(uint8_t *) &g_ether0_ether_buffer124[0],
(uint8_t *) &g_ether0_ether_buffer125[0],
(uint8_t *) &g_ether0_ether_buffer126[0],
(uint8_t *) &g_ether0_ether_buffer127[0],
};
            const ether_cfg_t g_ether0_cfg =
            {
                .channel            = 0,
                .zerocopy           = ETHER_ZEROCOPY_ENABLE,
                .multicast          = ETHER_MULTICAST_ENABLE,
                .promiscuous        = ETHER_PROMISCUOUS_DISABLE,
                .flow_control       = ETHER_FLOW_CONTROL_ENABLE,
                .padding            = ETHER_PADDING_DISABLE,
                .padding_offset     = 0,
                .broadcast_filter   = 0,
                .p_mac_address      = g_ether0_mac_address,

                .num_tx_descriptors = 64,
                .num_rx_descriptors = 128,

                .pp_ether_buffers   = pp_g_ether0_ether_buffers,

                .ether_buffer_size  = 1536,

                .irq                = FSP_INVALID_VECTOR,

                .p_callback         = user_ether0_callback,
                .p_context          = NULL,
                .p_extend           = &g_ether0_extended_cfg_t,
            };

/* Instance structure to use this module. */
const ether_instance_t g_ether0 =
{
    .p_ctrl        = &g_ether0_ctrl,
    .p_cfg         = &g_ether0_cfg,
    .p_api         = &g_ether_on_rmac,
};
void g_hal_init(void) {
g_common_init();
}
