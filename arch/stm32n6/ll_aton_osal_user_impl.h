/* TikuOS OSAL used by the pinned ST LL-ATON relocatable runtime. */
#ifndef TIKU_LL_ATON_OSAL_USER_IMPL_H_
#define TIKU_LL_ATON_OSAL_USER_IMPL_H_

#include <stdint.h>

typedef void (*tiku_llaton_irq_handler_t)(void);

void tiku_llaton_osal_init(void);
void tiku_llaton_osal_deinit(void);
void tiku_llaton_osal_install_irq(uint32_t line, tiku_llaton_irq_handler_t handler);
void tiku_llaton_osal_remove_irq(uint32_t line);
void tiku_llaton_osal_enable_irq(uint32_t line);
void tiku_llaton_osal_disable_irq(uint32_t line);
void tiku_llaton_osal_enter_cs(void);
void tiku_llaton_osal_exit_cs(void);
void tiku_llaton_osal_signal_event(void);

#define LL_ATON_OSAL_INIT()                         tiku_llaton_osal_init()
#define LL_ATON_OSAL_DEINIT()                       tiku_llaton_osal_deinit()
#define LL_ATON_OSAL_WFE()                          ((void)0)
#define LL_ATON_OSAL_SIGNAL_EVENT()                 tiku_llaton_osal_signal_event()
#define LL_ATON_OSAL_INSTALL_IRQ(line, handler)     tiku_llaton_osal_install_irq((line), (handler))
#define LL_ATON_OSAL_REMOVE_IRQ(line)               tiku_llaton_osal_remove_irq(line)
#define LL_ATON_OSAL_ENABLE_IRQ(line)               tiku_llaton_osal_enable_irq(line)
#define LL_ATON_OSAL_DISABLE_IRQ(line)              tiku_llaton_osal_disable_irq(line)
#define LL_ATON_OSAL_ENTER_CS()                     tiku_llaton_osal_enter_cs()
#define LL_ATON_OSAL_EXIT_CS()                      tiku_llaton_osal_exit_cs()
#define LL_ATON_OSAL_LOCK_ATON()                    ((void)0)
#define LL_ATON_OSAL_UNLOCK_ATON()                  ((void)0)
#define LL_ATON_OSAL_LOCK_MCU_CACHE()               ((void)0)
#define LL_ATON_OSAL_UNLOCK_MCU_CACHE()             ((void)0)
#define LL_ATON_OSAL_LOCK_NPU_CACHE()               ((void)0)
#define LL_ATON_OSAL_UNLOCK_NPU_CACHE()             ((void)0)

#endif
