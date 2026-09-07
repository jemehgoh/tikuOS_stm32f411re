/* TikuOS wrapper around the pinned ST LL-ATON relocatable runtime. */
#ifndef TIKU_STM32N6_NPU_LLATON_H_
#define TIKU_STM32N6_NPU_LLATON_H_

#include <stdint.h>

int tiku_npu_llaton_runtime_init(void);
void tiku_npu_llaton_irq_bridge(void);

/* Called by the board IRQ setup and the LL-ATON OSAL implementation. */
void tiku_llaton_osal_install_irq(uint32_t line, void (*handler)(void));
void tiku_llaton_osal_remove_irq(uint32_t line);
void tiku_llaton_osal_enable_irq(uint32_t line);
void tiku_llaton_osal_disable_irq(uint32_t line);

#endif
