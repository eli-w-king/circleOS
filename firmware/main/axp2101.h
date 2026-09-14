#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

typedef struct {
    i2c_master_dev_handle_t device;
} axp2101_t;

typedef struct {
    bool battery_present;
    bool external_power;
    bool charging;
    bool percentage_valid;
    uint8_t percentage;
} axp2101_battery_t;

esp_err_t axp2101_init(axp2101_t *pmu, i2c_master_bus_handle_t bus);
esp_err_t axp2101_poll_button(axp2101_t *pmu, bool *short_press);
esp_err_t axp2101_read_battery(axp2101_t *pmu, axp2101_battery_t *battery);
