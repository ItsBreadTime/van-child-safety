#pragma once

void sensor_sht4x_init(void);
void sensor_sht4x_read_and_update(void);
void sensor_i2c_task(void *pvParameters);
