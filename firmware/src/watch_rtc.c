#include "watch_rtc.h"
#include <rtthread.h>
#include <rthw.h>
#include <bf0_hal.h>
#include <system_bf0_ap.h>

/* SystemInit runs before the GCC copy/zero loops. The board linker places
 * this section in internal SRAM outside both tables. Overwrite it each boot. */
static struct {
    struct watch_rtc_sample calendar;
    uint32_t cr, wsr, backup, rtc_cr, prescaler;
    PMU_BootModeTypeDef mode;
} boot __attribute__((section(".bss.l1_non_ret_bss_watch_boot")));

void SystemPowerOnModeInit(void)
{
    boot.cr = hwp_pmuc->CR;
    boot.wsr = hwp_pmuc->WSR;
    boot.backup = hwp_rtc->BKP2R;  /* RTC_BACKUP_INITIALIZED, before drv_rtc */
    boot.rtc_cr = hwp_rtc->CR;
    boot.prescaler = hwp_rtc->PSCLR;
    boot.calendar.isr = hwp_rtc->ISR;
    boot.calendar.tr = hwp_rtc->TR;
    boot.calendar.dr = hwp_rtc->DR;
    uint32_t wake;
    HAL_PMU_CheckBootMode(&boot.mode, &wake); /* consumes the hardware flags */

    /* Power-key hold time, 32000 Hz ticks like the SDK board init. The same
     * counter also hard-resets a running chip, so it must stay well above the
     * ~3 s shutdown hold; 8 s is what power-on from hibernate takes anyway. */
    hwp_pmuc->PWRKEY_CNT = 32000u * 8u;
}

pm_power_on_mode_t SystemPowerOnModeGet(void)
{
    switch (boot.mode) {
    case PMU_HIBERNATE_BOOT: return PM_HIBERNATE_BOOT;
    case PMU_SHUTDOWN_BOOT: return PM_SHUTDOWN_BOOT;
    case PMU_REBOOT_BOOT: return PM_REBOOT_BOOT;
    default: return PM_COLD_BOOT;
    }
}

bool watch_rtc_domain_retained(void)
{
    return boot.backup != 0;
}

void watch_rtc_log_reset(void)
{
    const char *reason = boot.wsr & (PMUC_WSR_WDT1 | PMUC_WSR_WDT2 | PMUC_WSR_IWDT) ?
                         "watchdog" : "cold_or_external";
    switch (boot.mode) {
    case PMU_HIBERNATE_BOOT: reason = "hibernate"; break;
    case PMU_SHUTDOWN_BOOT: reason = "shutdown"; break;
    case PMU_REBOOT_BOOT: reason = "reboot"; break;
    default:
        if (boot.wsr & PMUC_WSR_PWRKEY)
            reason = "power_key_reset";
        break;
    }
    rt_kprintf("WATCH_RESET reason=%s pmu_cr=%08x wsr=%08x backup_init=%u pwrkey_cnt=%08x\n",
               reason, boot.cr, boot.wsr, boot.backup, hwp_pmuc->PWRKEY_CNT);
    rt_kprintf("WATCH_RTC_RAW tr=%08x dr=%08x isr=%08x cr=%08x psclr=%08x\n",
               boot.calendar.tr, boot.calendar.dr, boot.calendar.isr, boot.rtc_cr, boot.prescaler);
}

struct watch_rtc_sample watch_rtc_at_reset(void)
{
    return boot.calendar;
}

struct watch_rtc_sample watch_rtc_read(void)
{
    struct watch_rtc_sample s;
    /* The 52x HCPU mirror latches the date on TR read. DR unlocks it and
     * signals an inconsistent pair via ERR. Retry on a later poll. */
    rt_base_t level = rt_hw_interrupt_disable();
    s.isr = hwp_rtc->ISR;
    s.tr = hwp_hpsys_cfg->RTC_TR;
    s.dr = hwp_hpsys_cfg->RTC_DR;
    rt_hw_interrupt_enable(level);
    return s;
}

static unsigned bcd(uint32_t packed)
{
    unsigned units = packed & 15u;
    return units > 9 ? 255 : 10 * (packed >> 4) + units;
}

bool watch_rtc_valid(struct watch_rtc_sample s, uint32_t *epoch)
{
    if ((s.dr & RTC_DR_ERR) || (s.isr & RTC_ISR_INIT))
        return false;
    unsigned year = bcd((s.dr & (RTC_DR_YU | RTC_DR_YT)) >> RTC_DR_YU_Pos);
    unsigned month = bcd((s.dr & (RTC_DR_MU | RTC_DR_MT)) >> RTC_DR_MU_Pos);
    unsigned day = bcd((s.dr & (RTC_DR_DU | RTC_DR_DT)) >> RTC_DR_DU_Pos);
    unsigned hour = bcd((s.tr & (RTC_TR_HU | RTC_TR_HT)) >> RTC_TR_HU_Pos);
    unsigned min = bcd((s.tr & (RTC_TR_MNU | RTC_TR_MNT)) >> RTC_TR_MNU_Pos);
    unsigned sec = bcd((s.tr & (RTC_TR_SU | RTC_TR_ST)) >> RTC_TR_SU_Pos);
    if (year < 25 || year > 99 || month < 1 || month > 12 || hour > 23 || min > 59 || sec > 59)
        return false;
    /* Match the SDK's 1970 century pivot without modifying the DR register. */
    if ((s.dr & RTC_DR_CB) && year >= 70)
        return false;
    static const unsigned short days_before[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365};
    bool leap = year % 4 == 0;  /* supported years 2025..2099 */
    unsigned month_days = days_before[month] - days_before[month - 1] + (leap && month == 2);
    if (day < 1 || day > month_days)
        return false;
    /* The existing BLE protocol already includes the local UTC offset. */
    uint32_t days = year * 365 + (year + 3) / 4 + days_before[month - 1] +
                    (leap && month > 2) + day - 1;
    *epoch = 946684800u + days * 86400u + hour * 3600u + min * 60u + sec;
    return true;
}

void watch_rtc_hibernate(bool usb)
{
    /* On 52x RTC_CR.LPCKSEL selects the RTC crystal. The PMU LP mux is
     * separate. The BSP selects RC32 there during startup. */
    if (HAL_GPIO_ReadPin(hwp_gpio1, WATCH_POWER_KEY_PIN) != GPIO_PIN_RESET) {
        rt_kprintf("WATCH_HIBERNATE refused key_pressed\n");
        return;
    }
    if (!(hwp_pmuc->LXT_CR & PMUC_LXT_CR_RDY) || !HAL_RTC_LXT_ENABLED() ||
            (hwp_rtc->ISR & RTC_ISR_INIT) || !(hwp_pmuc->CR & PMUC_CR_SEL_LPCLK) ||
            HAL_PIN_Set(PAD_PA34, GPIO_A34, PIN_PULLDOWN, 1) != 0) {
        rt_kprintf("WATCH_HIBERNATE refused rtc_cr=%08x lxt=%08x pmu_cr=%08x\n",
                   hwp_rtc->CR, hwp_pmuc->LXT_CR, hwp_pmuc->CR);
        return;
    }
    /* KEY1 is the PMU power key too, so a long press wakes hibernate without
     * any WER bit. The PA34 level wake is retired: on USB power it read high
     * and booted the watch 11..42 s into sleep (WSR.PIN0). Clear it explicitly
     * because WER survives hibernate and an earlier build may have set it. */
    HAL_PMU_DisablePinWakeup(0);
    struct watch_rtc_sample sample = watch_rtc_read();
    uint32_t epoch = 0;
    watch_rtc_valid(sample, &epoch);
    rt_kprintf("WATCH_HIBERNATE enter vbus=%d wake=pwrkey rtc=lxt epoch=%u wer=%08x\n",
               usb, epoch, hwp_pmuc->WER);
    rt_hw_interrupt_disable();
    /* LDO3 powers the watch peripherals. The RTC stays on the always-on rail. */
    HAL_PMU_ConfigPeriLdo(PMU_PERI_LDO3_3V3, false, false);
    HAL_PMU_EnterHibernate();
}
