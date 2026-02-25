# ESP32 BME280 to Supabase (Half-hour schedule + Deep Sleep)

This project reads temperature and humidity from a BME280 sensor (I2C) and posts the data to Supabase using the REST endpoint.
It runs on an ESP32 with Arduino IDE and uses deep sleep to save battery.

## Features

- Posts sensor readings at exact half-hour boundaries:
  - HH:00 and HH:30 (e.g., 10:00, 10:30, 11:00, 11:30 ...)
- Uses NTP to keep accurate time
- Deep sleeps between readings to minimize power usage
- Uses a GPIO power pin for the sensor (optional)

## Hardware

- ESP32
- BME280 sensor (I2C)
- External battery supply (optional)

### Pin configuration (as used in the sketch)

```cpp
const uint8_t PIN_BME_SDA = 17;
const uint8_t PIN_BME_SCL = 16;
const uint8_t PIN_BME_PWR =  4;   // optional power rail
const uint8_t BME_ADDRESS = 0x76; // try 0x77 if needed