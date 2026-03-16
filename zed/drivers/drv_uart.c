/***********************************************************************************************************************
 * Includes
 **********************************************************************************************************************/
#include "drv_uart.h"
#include <string.h>


/**********************************************************************************************************************
 * Macro definitions
 **********************************************************************************************************************/
#define DRV_UART_RX_RING_SIZE    256U


/**********************************************************************************************************************
 * Typedef definitions
 **********************************************************************************************************************/


/***********************************************************************************************************************
 * Private function prototypes
 **********************************************************************************************************************/
static fsp_err_t drv_uart_arm_rx(void);
static void      drv_uart_push_rx_byte(uint8_t rx_byte);


/***********************************************************************************************************************
 * Private global variables
 **********************************************************************************************************************/
static volatile int      g_uart7_tx_complete = 0;
static volatile int      g_uart7_rx_complete = 0;
static volatile bool     g_uart7_rx_started = false;
static volatile uint8_t  g_uart7_rx_byte = 0U;
static volatile uint16_t g_uart7_rx_head = 0U;
static volatile uint16_t g_uart7_rx_tail = 0U;
static uint8_t           g_uart7_rx_ring[DRV_UART_RX_RING_SIZE];


/***********************************************************************************************************************
 * Functions
 **********************************************************************************************************************/

fsp_err_t drv_uart_init(void)
{
    fsp_err_t err;

    /* 鎵撳紑涓插彛 */
    err = g_uart7.p_api->open(g_uart7.p_ctrl, g_uart7.p_cfg);
    if (FSP_SUCCESS != err)
    {
        __BKPT();
    }

    if (FSP_SUCCESS == err)
    {
        err = drv_uart_start_rx();
    }

    return err;
}

fsp_err_t drv_uart_test(uint8_t *p_msg)
{
    fsp_err_t err;
    uint8_t msg_len = 0;
    char *p_temp_ptr = (char *)p_msg;

    /* 璁＄畻闀垮害 */
    msg_len = ((uint8_t)(strlen((char *)p_temp_ptr)));

    /* 鍚姩鍙戦€?*/
    err = g_uart7.p_api->write(g_uart7.p_ctrl, p_msg, msg_len);
    /* 绛夊緟鍙戦€佸畬姣?*/
    drv_uart_wait_for_tx();

    return err;
}

fsp_err_t drv_uart_start_rx(void)
{
    fsp_err_t err;

    if (g_uart7_rx_started)
    {
        return FSP_SUCCESS;
    }

    drv_uart_flush_rx();
    g_uart7_rx_complete = 0;
    g_uart7_rx_byte = 0U;
    err = drv_uart_arm_rx();
    if (FSP_SUCCESS == err)
    {
        g_uart7_rx_started = true;
    }

    return err;
}

void drv_uart_flush_rx(void)
{
    g_uart7_rx_head = 0U;
    g_uart7_rx_tail = 0U;
    memset(g_uart7_rx_ring, 0, sizeof(g_uart7_rx_ring));
}

bool drv_uart_read_line(char * p_buffer, size_t buffer_size)
{
    uint16_t scan;
    size_t   out_len = 0U;

    if ((NULL == p_buffer) || (buffer_size < 2U))
    {
        return false;
    }

    scan = g_uart7_rx_tail;
    while (scan != g_uart7_rx_head)
    {
        uint8_t ch = g_uart7_rx_ring[scan];

        if (('\n' == ch) || ('\r' == ch))
        {
            break;
        }

        scan = (uint16_t) ((scan + 1U) % DRV_UART_RX_RING_SIZE);
    }

    if (scan == g_uart7_rx_head)
    {
        return false;
    }

    while (g_uart7_rx_tail != g_uart7_rx_head)
    {
        uint8_t ch = g_uart7_rx_ring[g_uart7_rx_tail];

        g_uart7_rx_tail = (uint16_t) ((g_uart7_rx_tail + 1U) % DRV_UART_RX_RING_SIZE);

        if (('\n' == ch) || ('\r' == ch))
        {
            if (('\r' == ch) &&
                (g_uart7_rx_tail != g_uart7_rx_head) &&
                ('\n' == g_uart7_rx_ring[g_uart7_rx_tail]))
            {
                g_uart7_rx_tail = (uint16_t) ((g_uart7_rx_tail + 1U) % DRV_UART_RX_RING_SIZE);
            }
            break;
        }

        if ((out_len + 1U) < buffer_size)
        {
            p_buffer[out_len++] = (char) ch;
        }
    }

    p_buffer[out_len] = '\0';

    return true;
}

void drv_uart_wait_for_tx(void)
{
    while (!g_uart7_tx_complete) // 闃诲绛夊緟
    {
    }

    g_uart7_tx_complete = 0;
}

void drv_uart_wait_for_rx(void)
{
    while (!g_uart7_rx_complete) // 闃诲绛夊緟
    {
    }

    g_uart7_rx_complete = 0;
}


void uart7_callback(uart_callback_args_t * p_args)
{
    switch (p_args->event)
    {
        case UART_EVENT_TX_COMPLETE:
        {
            g_uart7_tx_complete = 1;
            break;
        }

        case UART_EVENT_RX_COMPLETE:
        {
            drv_uart_push_rx_byte(g_uart7_rx_byte);
            g_uart7_rx_complete = 1;
            (void) drv_uart_arm_rx();
            break;
        }

        default:
        {
            break;
        }
    }
}

/*printf杈撳嚭閲嶅畾鍚戝埌涓插彛*/
int __io_putchar(int ch)
{
    fsp_err_t err = FSP_SUCCESS;

    err = g_uart7.p_api->write(g_uart7.p_ctrl, (uint8_t*)&ch, 1);

    if (FSP_SUCCESS != err)
    {
        __BKPT();
    }

    drv_uart_wait_for_tx();

    return ch;
}

int _write(int fd, char *pBuffer, int size)
{
    ((void)fd);

    for (int i = 0; i < size; i++)
    {
        __io_putchar(*pBuffer++);
    }

    return size;
}

/***********************************************************************************************************************
 * Private Functions
 **********************************************************************************************************************/
static fsp_err_t drv_uart_arm_rx(void)
{
    return g_uart7.p_api->read(g_uart7.p_ctrl, (uint8_t *) &g_uart7_rx_byte, 1U);
}

static void drv_uart_push_rx_byte(uint8_t rx_byte)
{
    uint16_t next_head = (uint16_t) ((g_uart7_rx_head + 1U) % DRV_UART_RX_RING_SIZE);

    if (next_head == g_uart7_rx_tail)
    {
        g_uart7_rx_tail = (uint16_t) ((g_uart7_rx_tail + 1U) % DRV_UART_RX_RING_SIZE);
    }

    g_uart7_rx_ring[g_uart7_rx_head] = rx_byte;
    g_uart7_rx_head = next_head;
}
