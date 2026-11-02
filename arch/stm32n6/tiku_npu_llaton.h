/* TikuOS wrapper around the pinned ST LL-ATON relocatable runtime. */
#ifndef TIKU_STM32N6_NPU_LLATON_H_
#define TIKU_STM32N6_NPU_LLATON_H_

#include <stddef.h>
#include <stdint.h>

int tiku_npu_llaton_runtime_init(void);
int tiku_npu_llaton_runtime_deinit(void);
void tiku_npu_llaton_irq_bridge(void);

/* Called by the board IRQ setup and the LL-ATON OSAL implementation. */
void tiku_llaton_osal_install_irq(uint32_t line, void (*handler)(void));
void tiku_llaton_osal_remove_irq(uint32_t line);
void tiku_llaton_osal_enable_irq(uint32_t line);
void tiku_llaton_osal_disable_irq(uint32_t line);

#if defined(TIKU_NPU_RUN_TEST_ENABLE)
typedef struct {
    uint32_t submissions;
    uint32_t reset_calls;
    uint32_t epoch_calls;
    uint32_t wfe_returns;
    uint32_t no_wfe_returns;
    uint32_t done_returns;
    uint32_t runtime_failures;
    uint32_t wake_posts;
    uint32_t worker_wake_posts;
    uint32_t irq_wake_posts;
    uint32_t dropped_wake_posts;
    uint32_t completion_posts;
    uint32_t dropped_completion_posts;
    uint32_t ignored_wake_events;
    uint32_t reset_order_failures;
    int last_submit_result;
} tiku_npu_llaton_run_trace_t;

void tiku_npu_llaton_test_reset_run_trace(void);
int tiku_npu_llaton_test_get_run_trace(tiku_npu_llaton_run_trace_t *out);
#endif

#if defined(TIKU_NPU_PREFLIGHT_TEST_ENABLE)
/* Test-only seam for exercising the bounded parser without entering LL-ATON. */
int tiku_npu_llaton_test_image_preflight(const void *image, size_t length,
                                         uintptr_t params_ptr,
                                         size_t params_length);
#endif

#if defined(TIKU_NPU_POOL_TEST_ENABLE)
/* Test-only seams for pool policy validation and allocation without install. */
int tiku_npu_llaton_test_validate_pools(const void *image, size_t length,
                                        uintptr_t params_ptr,
                                        size_t params_length,
                                        uint32_t params_offset,
                                        uint32_t ext_ram_size);
int tiku_npu_llaton_test_materialize_pools(const void *image, size_t length,
                                           uintptr_t params_ptr,
                                           size_t params_length,
                                           uint32_t params_offset,
                                           uint32_t exec_ram_size,
                                           uint32_t ext_ram_size);
#endif

#if defined(TIKU_NPU_RESOURCE_TEST_ENABLE)
/* Test-only seams for exercising resource sizing without installing a model. */
int tiku_npu_llaton_test_reserve_resources(uint32_t exec_size,
                                           uint32_t ext_size,
                                           uintptr_t *exec_addr,
                                           uintptr_t *ext_addr);
#endif

#if defined(TIKU_NPU_RESOURCE_TEST_ENABLE) || \
    defined(TIKU_NPU_POOL_TEST_ENABLE) || \
    defined(TIKU_NPU_LIFECYCLE_TEST_ENABLE)
#define TIKU_NPU_LLATON_TEST_MAX_FIXED_REGIONS 10u

typedef struct {
    uintptr_t exec_addr;
    uintptr_t ext_addr;
    uintptr_t fixed_addr[TIKU_NPU_LLATON_TEST_MAX_FIXED_REGIONS];
    size_t fixed_size[TIKU_NPU_LLATON_TEST_MAX_FIXED_REGIONS];
    size_t root_used;
    uint8_t exec_active;
    uint8_t ext_active;
    uint8_t fixed_count;
    uint8_t fixed_active_count;
} tiku_npu_llaton_resource_trace_t;

unsigned tiku_npu_llaton_test_ext_ram_reserve_attempts(void);
unsigned tiku_npu_llaton_test_install_calls(void);
int tiku_npu_llaton_test_release_resources(void);
int tiku_npu_llaton_test_get_resource_trace(
    tiku_npu_llaton_resource_trace_t *out);
#endif

#if defined(TIKU_NPU_RESOURCE_TEST_ENABLE)
int tiku_npu_llaton_test_get_config(uintptr_t *exec_addr,
                                    uint32_t *exec_size,
                                    uintptr_t *ext_addr,
                                    size_t *ext_size,
                                    uintptr_t *param_addr,
                                    uint32_t *required_exec_size,
                                    uint32_t *requested_ext_size);
#endif

#if defined(TIKU_NPU_LIFECYCLE_TEST_ENABLE)
enum {
    TIKU_NPU_LIFECYCLE_FAIL_NONE = 0,
    TIKU_NPU_LIFECYCLE_FAIL_RESERVE,
    TIKU_NPU_LIFECYCLE_FAIL_AFTER_EXT,
    TIKU_NPU_LIFECYCLE_FAIL_AFTER_FIXED,
    TIKU_NPU_LIFECYCLE_FAIL_INSTALL,
    TIKU_NPU_LIFECYCLE_FAIL_AFTER_INIT
};

typedef struct {
    unsigned network_deinit_calls;
    unsigned install_calls;
    uintptr_t exec_addr;
    uintptr_t ext_addr;
    uintptr_t fixed_addr[TIKU_NPU_LLATON_TEST_MAX_FIXED_REGIONS];
    size_t fixed_size[TIKU_NPU_LLATON_TEST_MAX_FIXED_REGIONS];
    size_t root_used;
    uint8_t runtime_initialized;
    uint8_t exec_active;
    uint8_t ext_active;
    uint8_t fixed_count;
    uint8_t fixed_active_count;
    uint8_t instance_installed;
    uint8_t network_initialized;
} tiku_npu_llaton_lifecycle_trace_t;

void tiku_npu_llaton_test_reset_lifecycle(void);
void tiku_npu_llaton_test_set_load_failure(int failure);
int tiku_npu_llaton_test_get_lifecycle_trace(
    tiku_npu_llaton_lifecycle_trace_t *out);
#endif

#endif
