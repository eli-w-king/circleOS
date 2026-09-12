#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

typedef struct {
    i2c_master_dev_handle_t device;
} qmi8658_t;

esp_err_t qmi8658_init(qmi8658_t *imu, i2c_master_bus_handle_t bus);
esp_err_t qmi8658_read_gyro_z(qmi8658_t *imu, float *degrees_per_second);
