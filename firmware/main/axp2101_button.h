#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

typedef struct {
    i2c_master_dev_handle_t device;
} axp2101_button_t;

esp_err_t axp2101_button_init(
    axp2101_button_t *button,
    i2c_master_bus_handle_t bus);
esp_err_t axp2101_button_poll(
    axp2101_button_t *button,
    bool *short_press);
