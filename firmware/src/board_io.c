#include "watch_board.h"
#include <board.h>
#include <rtdevice.h>
#include "bf0_hal.h"
#include "drivers/rt_drv_pwm.h"

#ifndef WATCH_LOG_VIB_EDGES
#define WATCH_LOG_VIB_EDGES 1
#endif
#define WATCH_VIB_PWM_PERIOD_NS 1000000u

static struct rt_i2c_bus_device *imu_bus;
static uint16_t imu_addr;

static int imu_read_reg(uint8_t reg, uint8_t *value, unsigned length)
{
    struct rt_i2c_msg ops[] = {
        {.addr = imu_addr, .flags = RT_I2C_WR, .buf = &reg, .len = 1},
        {.addr = imu_addr, .flags = RT_I2C_RD, .buf = value, .len = length},
    };
    return rt_i2c_transfer(imu_bus, ops, 2) == 2 ? 0 : -1;
}

static int imu_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t bytes[] = {reg, value};
    struct rt_i2c_msg op = {.addr = imu_addr, .flags = RT_I2C_WR, .buf = bytes, .len = 2};
    return rt_i2c_transfer(imu_bus, &op, 1) == 1 ? 0 : -1;
}

int watch_imu_init(void)
{
    /* Keep the LCD touch controller on I2C1. Route the IMU bus to I2C3. */
    HAL_PIN_Set(PAD_PA39, I2C3_SDA, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_PA40, I2C3_SCL, PIN_PULLUP, 1);
    imu_bus = rt_i2c_bus_device_find("i2c3");
    if (!imu_bus)
        return -1;
    uint8_t who = 0;
    for (imu_addr = 0x6a; imu_addr <= 0x6b; ++imu_addr)
        if (!imu_read_reg(0x0f, &who, 1) && who == 0x6a) {
            /* BDU + IF_INC. Accelerometer 52 Hz, +/-16 g. Gyroscope 52 Hz, +/-2000 dps. */
            if (imu_write_reg(0x12, 0x44) || imu_write_reg(0x10, 0x34) ||
                imu_write_reg(0x11, 0x3c))
                return -1;
            return 0;
        }
    return -1;
}

static int16_t axis(const uint8_t *raw)
{
    return (int16_t)((uint16_t)raw[0] | (uint16_t)raw[1] << 8);
}

bool watch_imu_read(struct watch_imu *sample)
{
    uint8_t status, raw[12];
    if (imu_read_reg(0x1e, &status, 1) || (status & 3) != 3 || imu_read_reg(0x22, raw, 12))
        return false;
    *sample = (struct watch_imu){
        axis(raw + 6) * 488 / 1000, axis(raw + 8) * 488 / 1000, axis(raw + 10) * 488 / 1000,
        axis(raw) * 7 / 10, axis(raw + 2) * 7 / 10, axis(raw + 4) * 7 / 10,
    };
    return true;
}

void watch_motor(bool on, uint8_t duty)
{
    static bool requested, running, owns_rail;
    static struct rt_device_pwm *pwm;
    on = on && duty != 0;
    if (on == requested)
        return;
    requested = on;
    if (on) {
        pwm = (struct rt_device_pwm *)rt_device_find("pwmt1");
        if (!pwm) {
            rt_kprintf("WATCH_VIB pwm device missing\n");
            return;
        }
        owns_rail = !(hwp_pmuc->PERI_LDO & PMUC_PERI_LDO_EN_VDD33_LDO3);
        if (HAL_PMU_ConfigPeriLdo(PMU_PERI_LDO3_3V3, true, true) != HAL_OK) {
            rt_kprintf("WATCH_VIB rail enable failed\n");
            return;
        }
        HAL_PIN_Set(PAD_PA20, GPTIM1_CH2, PIN_NOPULL, 1);
        uint32_t pulse = WATCH_VIB_PWM_PERIOD_NS * (duty > 100u ? 100u : duty) / 100u;
        if (rt_pwm_set(pwm, 2, WATCH_VIB_PWM_PERIOD_NS, pulse) != RT_EOK || rt_pwm_enable(pwm, 2) != RT_EOK) {
            HAL_PIN_Set(PAD_PA20, GPIO_A20, PIN_PULLDOWN, 1);
            if (owns_rail)
                HAL_PMU_ConfigPeriLdo(PMU_PERI_LDO3_3V3, false, false);
            rt_kprintf("WATCH_VIB pwm enable failed\n");
            return;
        }
        if (WATCH_LOG_VIB_EDGES)
            rt_kprintf("WATCH_VIB on ms=%u\n", (unsigned)watch_now_ms());
    } else if (running) {
        rt_pwm_disable(pwm, 2);
        HAL_PIN_Set(PAD_PA20, GPIO_A20, PIN_PULLDOWN, 1);
        rt_pin_mode(20, PIN_MODE_INPUT_PULLDOWN);
        if (owns_rail)
            HAL_PMU_ConfigPeriLdo(PMU_PERI_LDO3_3V3, false, false);
        if (WATCH_LOG_VIB_EDGES)
            rt_kprintf("WATCH_VIB off ms=%u\n", (unsigned)watch_now_ms());
    }
    running = on;
}
