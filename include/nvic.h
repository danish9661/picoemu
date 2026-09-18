#ifndef NVIC_H
#define NVIC_H

#include <stdint.h>

/* ========================================================================
 * ARM Cortex-M0+ NVIC (Nested Vectored Interrupt Controller)
 * ======================================================================== */

/* NVIC Base Address */
#define NVIC_BASE                   0xE000E000

/* NVIC Registers */
#define NVIC_ISER                   (NVIC_BASE + 0x100)   /* Interrupt Set Enable Register */
#define NVIC_ICER                   (NVIC_BASE + 0x180)   /* Interrupt Clear Enable Register */
#define NVIC_ISPR                   (NVIC_BASE + 0x200)   /* Interrupt Set Pending Register */
#define NVIC_ICPR                   (NVIC_BASE + 0x280)   /* Interrupt Clear Pending Register */
#define NVIC_IABR                   (NVIC_BASE + 0x300)  /* IRQ Active Bit Register */
#define NVIC_IPR                    (NVIC_BASE + 0x400)   /* Interrupt Priority Register (0-7) */

/* SysTick Registers (0xE000E010 - 0xE000E01F) */
#define SYST_CSR                    (NVIC_BASE + 0x010)   /* SysTick Control and Status */
#define SYST_RVR                    (NVIC_BASE + 0x014)   /* SysTick Reload Value */
#define SYST_CVR                    (NVIC_BASE + 0x018)   /* SysTick Current Value */
#define SYST_CALIB                  (NVIC_BASE + 0x01C)   /* SysTick Calibration */

/* System Handler Base */
#define SCB_BASE                    (NVIC_BASE + 0xD00)
#define SCB_ICSR                    (SCB_BASE + 0x04)     /* Interrupt Control and State Register */
#define SCB_VTOR                    (SCB_BASE + 0x08)     /* Vector Table Offset Register */
#define SCB_AIRCR                   (SCB_BASE + 0x0C)     /* Application Interrupt and Reset Control */
#define SCB_SCR                     (SCB_BASE + 0x10)     /* System Control Register */
#define SCB_CCR                     (SCB_BASE + 0x14)     /* Configuration and Control */
#define SCB_SHPR1                   (SCB_BASE + 0x18)     /* System Handler Priority 1 (MemManage/BusFault/UsageFault, M33) */
#define SCB_SHPR2                   (NVIC_BASE + 0xD1C)   /* System Handler Priority 2 (SVCall) */
#define SCB_SHPR3                   (NVIC_BASE + 0xD20)   /* System Handler Priority 3 (PendSV, SysTick) */
#define SCB_SHCSR                   (SCB_BASE + 0x24)     /* System Handler Control and State (M33 faults) */
#define SCB_CFSR                    (SCB_BASE + 0x28)     /* Configurable Fault Status (M33 MemManage/BusFault/UsageFault) */
#define SCB_HFSR                    (SCB_BASE + 0x2C)     /* HardFault Status (M33) */
#define SCB_MMFAR                   (SCB_BASE + 0x34)     /* MemManage Fault Address (M33) */
#define SCB_BFAR                    (SCB_BASE + 0x38)     /* BusFault Address (M33) */

/* MPU (M33, ARMv8-M, 8 regions; base 0xE000ED90) */
#define MPU_BASE                    (NVIC_BASE + 0xD90)
#define MPU_TYPE                    (MPU_BASE + 0x00)
#define MPU_CTRL                    (MPU_BASE + 0x04)
#define MPU_RNR                     (MPU_BASE + 0x08)
#define MPU_RBAR                    (MPU_BASE + 0x0C)
#define MPU_RLAR                    (MPU_BASE + 0x10)
#define MPU_RBAR_A1                 (MPU_BASE + 0x14)
#define MPU_RLAR_A1                 (MPU_BASE + 0x18)
#define MPU_RBAR_A2                 (MPU_BASE + 0x1C)
#define MPU_RLAR_A2                 (MPU_BASE + 0x20)
#define MPU_RBAR_A3                 (MPU_BASE + 0x24)
#define MPU_RLAR_A3                 (MPU_BASE + 0x28)
#define MPU_MAIR0                   (MPU_BASE + 0x30)
#define MPU_MAIR1                   (MPU_BASE + 0x34)
#define MPU_TYPE_DREGION            8
#define MPU_CTRL_ENABLE             (1u << 0)
#define MPU_CTRL_HFNMIENA           (1u << 1)
#define MPU_CTRL_PRIVDEFENA         (1u << 2)
#define MPU_RLAR_EN                 (1u << 0)

/* SAU (M33 TrustZone address attribution; base 0xE000EDD0, 8 regions) */
#define SAU_BASE                    (NVIC_BASE + 0xDD0)
#define SAU_CTRL                    (SAU_BASE + 0x00)
#define SAU_TYPE                    (SAU_BASE + 0x04)
#define SAU_RNR                     (SAU_BASE + 0x08)
#define SAU_RBAR                    (SAU_BASE + 0x0C)
#define SAU_RLAR                    (SAU_BASE + 0x10)
#define SAU_TYPE_SREGION            8
#define SAU_CTRL_ENABLE             (1u << 0)
#define SAU_CTRL_ALLNS              (1u << 1)
#define SAU_RLAR_ENABLE             (1u << 0)
#define SAU_RLAR_NSC                (1u << 1)

/* M33 fault exceptions (enabled via SHCSR, pended in CFSR/HFSR) */
#define EXC_MEMFAULT                4
#define EXC_BUSFAULT                5
#define EXC_USAGEFAULT              6

/* System Handlers (Exceptions 0-15) */
#define EXC_RESET                   1
#define EXC_NMI                     2
#define EXC_HARDFAULT               3
#define EXC_SVCALL                  11
#define EXC_PENDSV                  14
#define EXC_SYSTICK                 15

/* Cortex-M0+ Interrupt Sources (16-31) */
#define IRQ_TIMER_IRQ_0             0      /* TIMER_IRQ_0 */
#define IRQ_TIMER_IRQ_1             1      /* TIMER_IRQ_1 */
#define IRQ_TIMER_IRQ_2             2      /* TIMER_IRQ_2 */
#define IRQ_TIMER_IRQ_3             3      /* TIMER_IRQ_3 */
#define IRQ_PWM_IRQ_WRAP            4      /* PWM_IRQ_WRAP */
#define IRQ_USBCTRL_IRQ_ACPI        5      /* USBCTRL_IRQ */
#define IRQ_XIP_IRQ                 6      /* XIP_IRQ */
#define IRQ_PIO0_IRQ_0              7      /* PIO0_IRQ_0 */
#define IRQ_PIO0_IRQ_1              8      /* PIO0_IRQ_1 */
#define IRQ_PIO1_IRQ_0              9      /* PIO1_IRQ_0 */
#define IRQ_PIO1_IRQ_1              10     /* PIO1_IRQ_1 */
#define IRQ_DMA_IRQ_0               11     /* DMA_IRQ_0 */
#define IRQ_DMA_IRQ_1               12     /* DMA_IRQ_1 */
#define IRQ_IO_IRQ_BANK0            13     /* IO_IRQ_BANK0 */
#define IRQ_IO_IRQ_QSPI             14     /* IO_IRQ_QSPI */
#define IRQ_SIO_IRQ_PROC0           15     /* SIO_IRQ_PROC0 */
#define IRQ_SIO_IRQ_PROC1           16     /* SIO_IRQ_PROC1 */
#define IRQ_CLOCKS_IRQ              17     /* CLOCKS_IRQ */
#define IRQ_SPI0_IRQ                18     /* SPI0_IRQ */
#define IRQ_SPI1_IRQ                19     /* SPI1_IRQ */
#define IRQ_UART0_IRQ               20     /* UART0_IRQ */
#define IRQ_UART1_IRQ               21     /* UART1_IRQ */
#define IRQ_ADC_IRQ_FIFO            22     /* ADC_IRQ_FIFO */
#define IRQ_I2C0_IRQ                23     /* I2C0_IRQ */
#define IRQ_I2C1_IRQ                24     /* I2C1_IRQ */
#define IRQ_RTC_IRQ                 25     /* RTC_IRQ */

#define NUM_EXTERNAL_IRQS           32     /* 26 wired on RP2040 + user IRQs 26-31
                                          * (software-pended via ISPR; the Pico
                                          * SDK claims them for background tasks
                                          * such as USB tud_task pumping) */
#define NUM_EXCEPTIONS              16     /* System exceptions (0-15) */
#define NUM_TOTAL_IRQS              (NUM_EXCEPTIONS + NUM_EXTERNAL_IRQS)  /* 48 total */

/* Priority levels (Cortex-M0+ supports 4 levels, using bits 6-7 of IPR) */
#define IRQ_PRIORITY_0              0      /* Highest priority */
#define IRQ_PRIORITY_1              1
#define IRQ_PRIORITY_2              2
#define IRQ_PRIORITY_3              3      /* Lowest priority */

/* ICSR Register Bits */
#define ICSR_VECTPENDING_SHIFT      12
#define ICSR_VECTPENDING_MASK       0xFF
#define ICSR_ISRPENDING             (1 << 22)
#define ICSR_ISRPREEMPT             (1 << 23)
#define ICSR_PENDSVSET              (1 << 28)
#define ICSR_PENDSVCLR              (1 << 29)
#define ICSR_PENDSTSET              (1 << 30)
#define ICSR_PENDSTCLR              (1 << 31)

/* SysTick State */
typedef struct {
    uint32_t csr;               /* Control and Status Register */
    uint32_t rvr;               /* Reload Value Register (24-bit) */
    uint32_t cvr;               /* Current Value Register (24-bit) */
    int pending;                /* SysTick exception pending */
    int zero_fired;             /* Reload==0 already fired (fire once, like silicon) */
} systick_state_t;

/* NVIC State Structure */
typedef struct {
    uint32_t enable;                /* ISER - Interrupt enable bits */
    uint32_t pending;               /* ISPR - Pending interrupt bits */
    uint8_t priority[NUM_EXTERNAL_IRQS];  /* IPR - Priority for each IRQ */
    uint32_t active_exceptions;     /* Bitmask of currently executing exceptions */
    uint32_t iabr;                  /* IABR - Active Bit Register */
    uint32_t shpr1;                 /* SHPR1 (M33 fault priorities) */
    uint32_t shpr2;                 /* System Handler Priority 2 (SVCall) */
    uint32_t shpr3;                 /* System Handler Priority 3 (PendSV, SysTick) */
    int pendsv_pending;             /* PendSV exception pending */
    int priorities_nondefault;      /* Nonzero if any IRQ priority != 0 (enables fast CTZ path) */
    uint32_t shcsr;                 /* SHCSR: fault exception enables/pended (M33) */
    uint32_t cfsr;                  /* CFSR: MemManage/BusFault/UsageFault status (M33) */
    uint32_t hfsr;                  /* HFSR: HardFault status incl. FORCED (M33) */
    uint32_t mmfar;                 /* MMFAR/BFAR fault addresses (M33) */
    uint32_t bfar;
} nvic_state_t;

/* M33 MPU state: 8 regions, PMSAv8 RBAR/RLAR encoding. */
typedef struct {
    uint32_t ctrl;
    uint32_t rnr;
    uint32_t rbar[8];
    uint32_t rlar[8];
    uint32_t mair[2];
} mpu_state_t;

/* M33 SAU state: 8 regions, attribution only (Secure / Non-secure /
 * Non-secure-callable). The emulator runs one Secure world; SAU answers
 * TT and reports attribution without splitting memory. */
typedef struct {
    uint32_t ctrl;
    uint32_t rnr;
    uint32_t rbar[8];
    uint32_t rlar[8];
} sau_state_t;

extern mpu_state_t mpu_state;
extern sau_state_t sau_state;

/* MPU check: 0 = access allowed, else fault exception to raise
 * (EXC_MEMFAULT normally, EXC_HARDFAULT when MPU disabled and the access
 * is to the PPB or when PRIVDEFENA denies unprivileged access). */
int mpu_check(uint32_t addr, int is_write, int is_priv, int is_exec);
/* SAU attribution: 0 = Secure, 1 = Non-secure, 2 = NSC. */
int sau_attr(uint32_t addr);
/* TT answer word for a TT/TTT/TTE instruction (I/E/M bits). */
uint32_t tt_answer(uint32_t addr, int alt);
/* Raise an M33 fault: sets CFSR/HFSR bits, pends the exception. */
void nvic_raise_fault(uint32_t exc, uint32_t cfsr_bits);

/* CPUID value (set by architecture overlay) */
extern uint32_t nvic_cpuid_value;

/* Functions */
void nvic_init(void);
void nvic_reset(void);

/* Interrupt control */
void nvic_enable_irq(uint32_t irq);
void nvic_disable_irq(uint32_t irq);
void nvic_set_pending(uint32_t irq);
void nvic_clear_pending(uint32_t irq);
void nvic_set_priority(uint32_t irq, uint8_t priority);

/* Interrupt processing */
uint32_t nvic_get_pending_irq(void);
uint32_t nvic_read_register(uint32_t addr);
void nvic_write_register(uint32_t addr, uint32_t val);

/* Signal from peripherals that interrupt occurred */
void nvic_signal_irq(uint32_t irq);

/* Get effective priority of an exception vector number */
uint8_t nvic_get_exception_priority(uint32_t vector_num);

/* SysTick functions */
void systick_init(void);
void systick_reset(void);
void systick_tick(uint32_t cycles);
void systick_tick_for_core(int core_id, uint32_t cycles);

/* Per-core NVIC and SysTick (RP2040 has independent NVIC per core) */
extern nvic_state_t nvic_states[2];
extern systick_state_t systick_states[2];

#endif /* NVIC_H */
