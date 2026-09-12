#include "axp2101_button.h"

#include <stddef.h>
#include <stdint.h>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"

#define AXP2101_ADDRESS 0x34
#define AXP2101_INTERRUPT_ENABLE_2 0x41
#define AXP2101_INTERRUPT_STATUS_2 0x49
#define AXP2101_POWER_KEY_LONG_BIT (1U << 2)
#define AXP2101_POWER_KEY_SHORT_BIT (1U << 3)
#define AXP2101_POWER_KEY_BITS \
    (AXP2101_POWER_KEY_LONG_BIT | AXP2101_POWER_KEY_SHORT_BIT)

static const char *TAG = "axp2101_button";

static esp_err_t read_register(
    axp2101_button_t *button,
    uint8_t register_address,
    uint8_t *value)
{
    return i2c_master_transmit_receive(
        button->device,
        &register_address,
        sizeof(register_address),
        value,
        sizeof(*value),
        pdMS_TO_TICKS(100));
}

static esp_err_t write_register(
    axp2101_button_t *button,
    uint8_t register_address,
    uint8_t value)
{
    const uint8_t command[] = {register_address, value};
    return i2c_master_transmit(
        button->device,
        command,
        sizeof(command),
        pdMS_TO_TICKS(100));
}

esp_err_t axp2101_button_init(
    axp2101_button_t *button,
    i2c_master_bus_handle_t bus)
{
    if (button == NULL || bus == NULL) {
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
            &button->device),
        TAG,
        "Unable to add AXP2101");

    uint8_t enabled_interrupts = 0;
    ESP_RETURN_ON_ERROR(
        read_register(
            button,
            AXP2101_INTERRUPT_ENABLE_2,
            &enabled_interrupts),
        TAG,
        "Unable to read power-key interrupt enable");
    ESP_RETURN_ON_ERROR(
        write_register(
            button,
            AXP2101_INTERRUPT_ENABLE_2,
            enabled_interrupts | AXP2101_POWER_KEY_BITS),
        TAG,
        "Unable to enable power-key interrupts");
    return write_register(
        button,
        AXP2101_INTERRUPT_STATUS_2,
        AXP2101_POWER_KEY_BITS);
}

esp_err_t axp2101_button_poll(
    axp2101_button_t *button,
    bool *short_press)
{
    if (button == NULL || button->device == NULL || short_press == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t status = 0;
    ESP_RETURN_ON_ERROR(
        read_register(
            button,
            AXP2101_INTERRUPT_STATUS_2,
            &status),
        TAG,
        "Unable to read power-key status");
    *short_press = (status & AXP2101_POWER_KEY_SHORT_BIT) != 0;
    if ((status & AXP2101_POWER_KEY_BITS) != 0) {
        return write_register(
            button,
            AXP2101_INTERRUPT_STATUS_2,
            status & AXP2101_POWER_KEY_BITS);
    }
    return ESP_OK;
}
