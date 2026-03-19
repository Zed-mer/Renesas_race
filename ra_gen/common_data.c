/* generated common source file - do not edit */
#include "common_data.h"
icu_instance_ctrl_t g_external_irq9_ctrl;
const external_irq_cfg_t g_external_irq9_cfg =
{ .channel = 9,
  .trigger = EXTERNAL_IRQ_TRIG_RISING,
  .filter_enable = false,
  .pclk_div = EXTERNAL_IRQ_PCLK_DIV_BY_64,
  .p_callback = icu9_callback,
  /** If NULL then do not add & */
#if defined(NULL)
    .p_context           = NULL,
#else
  .p_context = &NULL,
#endif
  .p_extend = NULL,
  .ipl = (11),
#if defined(VECTOR_NUMBER_ICU_IRQ9)
    .irq                 = VECTOR_NUMBER_ICU_IRQ9,
#else
  .irq = FSP_INVALID_VECTOR,
#endif
        };
/* Instance structure to use this module. */
const external_irq_instance_t g_external_irq9 =
{ .p_ctrl = &g_external_irq9_ctrl, .p_cfg = &g_external_irq9_cfg, .p_api = &g_external_irq_on_icu };
icu_instance_ctrl_t g_external_irq6_ctrl;
const external_irq_cfg_t g_external_irq6_cfg =
{ .channel = 6,
  .trigger = EXTERNAL_IRQ_TRIG_FALLING,
  .filter_enable = false,
  .pclk_div = EXTERNAL_IRQ_PCLK_DIV_BY_64,
  .p_callback = botton6_callback,
  /** If NULL then do not add & */
#if defined(NULL)
    .p_context           = NULL,
#else
  .p_context = &NULL,
#endif
  .p_extend = NULL,
  .ipl = (12),
#if defined(VECTOR_NUMBER_ICU_IRQ6)
    .irq                 = VECTOR_NUMBER_ICU_IRQ6,
#else
  .irq = FSP_INVALID_VECTOR,
#endif
        };
/* Instance structure to use this module. */
const external_irq_instance_t g_external_irq6 =
{ .p_ctrl = &g_external_irq6_ctrl, .p_cfg = &g_external_irq6_cfg, .p_api = &g_external_irq_on_icu };
icu_instance_ctrl_t g_external_irq8_ctrl;
const external_irq_cfg_t g_external_irq8_cfg =
{ .channel = 8,
  .trigger = EXTERNAL_IRQ_TRIG_RISING,
  .filter_enable = false,
  .pclk_div = EXTERNAL_IRQ_PCLK_DIV_BY_64,
  .p_callback = icu8_callback,
  /** If NULL then do not add & */
#if defined(NULL)
    .p_context           = NULL,
#else
  .p_context = &NULL,
#endif
  .p_extend = NULL,
  .ipl = (10),
#if defined(VECTOR_NUMBER_ICU_IRQ8)
    .irq                 = VECTOR_NUMBER_ICU_IRQ8,
#else
  .irq = FSP_INVALID_VECTOR,
#endif
        };
/* Instance structure to use this module. */
const external_irq_instance_t g_external_irq8 =
{ .p_ctrl = &g_external_irq8_ctrl, .p_cfg = &g_external_irq8_cfg, .p_api = &g_external_irq_on_icu };
ioport_instance_ctrl_t g_ioport_ctrl;
const ioport_instance_t g_ioport =
{ .p_api = &g_ioport_on_ioport, .p_ctrl = &g_ioport_ctrl, .p_cfg = &g_bsp_pin_cfg, };
void g_common_init(void)
{
}
