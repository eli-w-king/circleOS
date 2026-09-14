#include "axp2101.h"

#include <stddef.h>
#include <stdint.h>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"

#define AXP2101_ADDRESS 0x34
#define AXP2101_POWER_STATUS 0x00
#define AXP2101_CHARGE_STATUS 0x01
#define AXP2101_MODULE_ENABLE 0x18
#define AXP2101_ADC_CHANNEL_ENABLE 0x30
#define AXP2101_INTERRUPT_ENABLE_2 0x41
#define AXP2101_INTERRUPT_STATUS_2 0x49
#define AXP2101_BATTERY_PERCENTAGE 0xA4

#define AXP2101_POWER_KEY_LONG_BIT (1U << 2)
#define AXP2101_POWER_KEY_SHORT_BIT (1U << 3)
#define AXP2101_POWER_KEY_BITS \
    (AXP2101_POWER_KEY_LONG_BIT | AXP2101_POWER_KEY_SHORT_BIT)
#define AXP2101_VBUS_GOOD_BIT (1U << 5)
#define AXP2101_BATTERY_PRESENT_BIT (1U << 3)
#define AXP2101_FUEL_GAUGE_ENABLE_BIT (1U << 3)
#define AXP2101_BATTERY_VOLTAGE_ADC_BIT (1U << 0)
#define AXP2101_CHARGE_STATE_MASK 0xE0
#define AXP2101_CHARGE_STATE_SHIFT 5
#define AXP2101_CHARGE_STATE_CHARGING 1
#define AXP2101_PERCENTAGE_MAXIMUM 100

static const char *TAG = "axp2101";

static esp_err_t read_register(
    axp2101_t *pmu,
    uint8_t register_address,
    uint8_t *value)
{
    return i2c_master_transmit_receive(
        pmu->device,
        &register_address,
        sizeof(register_address),
        value,
        sizeof(*value),
        pdMS_TO_TICKS(100));
}

static esp_err_t write_register(
    axp2101_t *pmu,
    uint8_t register_address,
    uint8_t value)
{
    const uint8_t command[] = {register_address, value};
    return i2c_master_transmit(
        pmu->device,
        command,
        sizeof(command),
        pdMS_TO_TICKS(100));
}

static esp_err_t set_register_bits(
    axp2101_t *pmu,
    uint8_t register_address,
    uint8_t bits)
{
    uint8_t value = 0;
    ESP_RETURN_ON_ERROR(
        read_register(pmu, register_address, &value),
        TAG,
        "Unable to read register 0x%02x",
        register_address);
    if ((value & bits) == bits) {
        return ESP_OK;
    }
    return write_register(pmu, register_address, value | bits);
}

esp_err_t axp2101_init(axp2101_t *pmu, i2c_master_bus_handle_t bus)
{
    if (pmu == NULL || bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const i2c_device_config_t configuration = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDRESS,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(
            bus,
            &configuration,
            &pmu->device),
        TAG,
        "Unable to add AXP2101");

    ESP_RETURN_ON_ERROR(
        set_register_bits(
            pmu,
            AXP2101_ADC_CHANNEL_ENABLE,
            AXP2101_BATTERY_VOLTAGE_ADC_BIT),
        TAG,
        "Unable to enable battery voltage measurement");
    ESP_RETURN_ON_ERROR(
        set_register_bits(
            pmu,
            AXP2101_MODULE_ENABLE,
            AXP2101_FUEL_GAUGE_ENABLE_BIT),
        TAG,
        "Unable to enable fuel gauge");

    uint8_t enabled_interrupts = 0;
    ESP_RETURN_ON_ERROR(
        read_register(
            pmu,
            AXP2101_INTERRUPT_ENABLE_2,
            &enabled_interrupts),
        TAG,
        "Unable to read power-key interrupt enable");
    ESP_RETURN_ON_ERROR(
        write_register(
            pmu,
            AXP2101_INTERRUPT_ENABLE_2,
            enabled_interrupts | AXP2101_POWER_KEY_BITS),
        TAG,
        "Unable to enable power-key interrupts");
    return write_register(
        pmu,
        AXP2101_INTERRUPT_STATUS_2,
        AXP2101_POWER_KEY_BITS);
}

esp_err_t axp2101_poll_button(axp2101_t *pmu, bool *short_press)
{
    if (pmu == NULL || pmu->device == NULL || short_press == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t status = 0;
    ESP_RETURN_ON_ERROR(
        read_register(
            pmu,
            AXP2101_INTERRUPT_STATUS_2,
            &status),
        TAG,
        "Unable to read power-key status");
    *short_press = (status & AXP2101_POWER_KEY_SHORT_BIT) != 0;
    if ((status & AXP2101_POWER_KEY_BITS) != 0) {
        return write_register(
            pmu,
            AXP2101_INTERRUPT_STATUS_2,
            status & AXP2101_POWER_KEY_BITS);
    }
    return ESP_OK;
}

esp_err_t axp2101_read_battery(axp2101_t *pmu, axp2101_battery_t *battery)
{
    if (pmu == NULL || pmu->device == NULL || battery == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t power_status = 0;
    ESP_RETURN_ON_ERROR(
        read_register(pmu, AXP2101_POWER_STATUS, &power_status),
        TAG,
        "Unable to read power status");

    uint8_t charge_status = 0;
    ESP_RETURN_ON_ERROR(
        read_register(pmu, AXP2101_CHARGE_STATUS, &charge_status),
        TAG,
        "Unable to read charge status");

    uint8_t percentage = 0;
    ESP_RETURN_ON_ERROR(
        read_register(pmu, AXP2101_BATTERY_PERCENTAGE, &percentage),
        TAG,
        "Unable to read battery percentage");

    battery->external_power = (power_status & AXP2101_VBUS_GOOD_BIT) != 0;
    battery->battery_present =
        (charge_status & AXP2101_BATTERY_PRESENT_BIT) != 0;
    battery->charging =
        ((charge_status & AXP2101_CHARGE_STATE_MASK) >>
             AXP2101_CHARGE_STATE_SHIFT) == AXP2101_CHARGE_STATE_CHARGING;
    battery->percentage_valid =
        battery->battery_present && percentage <= AXP2101_PERCENTAGE_MAXIMUM;
    battery->percentage = battery->percentage_valid ? percentage : 0;
    return ESP_OK;
}
