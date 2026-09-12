#include "qmi8658.h"

#include <stddef.h>
#include <stdint.h>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define QMI8658_ADDRESS 0x6B
#define QMI8658_WHO_AM_I 0x00
#define QMI8658_CTRL1 0x02
#define QMI8658_CTRL3 0x04
#define QMI8658_CTRL5 0x06
#define QMI8658_CTRL7 0x08
#define QMI8658_GYRO_Z_L 0x3F
#define QMI8658_RESET 0x60
#define QMI8658_EXPECTED_ID 0x05
#define QMI8658_GYRO_SENSITIVITY_512DPS 64.0f

static const char *TAG = "qmi8658";

static esp_err_t write_register(
    qmi8658_t *imu,
    uint8_t register_address,
    uint8_t value)
{
    const uint8_t command[] = {register_address, value};
    return i2c_master_transmit(
        imu->device,
        command,
        sizeof(command),
        pdMS_TO_TICKS(100));
}

static esp_err_t read_registers(
    qmi8658_t *imu,
    uint8_t register_address,
    uint8_t *data,
    size_t length)
{
    return i2c_master_transmit_receive(
        imu->device,
        &register_address,
        sizeof(register_address),
        data,
        length,
        pdMS_TO_TICKS(100));
}

esp_err_t qmi8658_init(qmi8658_t *imu, i2c_master_bus_handle_t bus)
{
    if (imu == NULL || bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const i2c_device_config_t device_configuration = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = QMI8658_ADDRESS,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(
            bus,
            &device_configuration,
            &imu->device),
        TAG,
        "Unable to add IMU to I2C bus");

    uint8_t chip_id = 0;
    ESP_RETURN_ON_ERROR(
        read_registers(imu, QMI8658_WHO_AM_I, &chip_id, sizeof(chip_id)),
        TAG,
        "Unable to read chip ID");
    if (chip_id != QMI8658_EXPECTED_ID) {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(
        write_register(imu, QMI8658_RESET, 0xB0),
        TAG,
        "Unable to reset IMU");
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_RETURN_ON_ERROR(
        write_register(imu, QMI8658_CTRL1, 0x40),
        TAG,
        "Unable to enable register auto-increment");
    ESP_RETURN_ON_ERROR(
        write_register(imu, QMI8658_CTRL3, 0x56),
        TAG,
        "Unable to configure gyroscope");
    ESP_RETURN_ON_ERROR(
        write_register(imu, QMI8658_CTRL5, 0x00),
        TAG,
        "Unable to configure gyroscope filter");
    ESP_RETURN_ON_ERROR(
        write_register(imu, QMI8658_CTRL7, 0x02),
        TAG,
        "Unable to enable gyroscope");
    return ESP_OK;
}

esp_err_t qmi8658_read_gyro_z(qmi8658_t *imu, float *degrees_per_second)
{
    if (imu == NULL || imu->device == NULL || degrees_per_second == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t raw[2];
    ESP_RETURN_ON_ERROR(
        read_registers(imu, QMI8658_GYRO_Z_L, raw, sizeof(raw)),
        TAG,
        "Unable to read gyroscope");
    const int16_t value = (int16_t)(((uint16_t)raw[1] << 8) | raw[0]);
    *degrees_per_second = value / QMI8658_GYRO_SENSITIVITY_512DPS;
    return ESP_OK;
}
