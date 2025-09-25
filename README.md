# WiFi_MaP

![wifi_map](https://github.com/carvilsi/flipper0-wifi-map/blob/main/wifi_map.png?raw=true)

An ESP32 and FlipperZero wifi mapping.

Check the ESP32 side at: [esp32-wifi-map](https://github.com/carvilsi/esp32-wifi-map)

And do not forget to have both sides on the last version.

## Install, etc.

Clone this repo at `applications_user/` folder under `flipperzero-firmware`

`$ git clone git@github.com:carvilsi/flipper0-wifi-map.git`

### Build and flash

`$ ./fbt launch APPSRC=wifi_map`

### Logs

`$ minicom -D /dev/ttyACM0 -b 230400`

## Connection FlipperZero and ESP32

| Flipper Zero GPIO |    ESP32    |
|-------------------|-------------|
|      GND          |     GND     |
|      RX           |     TX0     |
|      3V3          |     3V3     |

## Collected Data

WiFi_MaP, by now, saves the collected data on a kind of **CSV** formated file, like:

```
##### 20-9-2024_17:47:47 #####
f8a;1.96;3;18
394;17.78;3;18
bab;34.81;3;18
2e4;34.81;4;18
f81;42.17;3;18
287;68.13;5;18
f1d;68.13;4;18
6cb;74.99;4;18
7b2;74.99;4;18
163;82.54;4;18
198;90.85;4;18
ff5;90.85;3;18
72a;100.00;4;18
163;100.00;4;18
f8a;1.78;3;21
```
Every time that WiFi_MaP starts adds the date to the file surronded by hash signs, then starts to add the collected AP
data separated by ';'. The maximun AP that scans is 20.
The data is:

| AP name hashed | distance to AP (meters) | AP auth mode (1) | seconds since the scan started | 
|----------------|-------------------------|------------------|--------------------------------|
|      f8a       |          1.96           |         3        |               18               |


**(1)** -> Table of ESP32 AP WIFI Auth Mode [wifi_auth_mode_t](https://github.com/pycom/esp-idf-2.0/blob/092aa8176ffa0ab386fb6d33e50e1a267bef9d1c/components/esp32/include/esp_wifi_types.h#L58)

| Open | WEP | WPA_PSK | WPA2_PSK | WPA_WPA2_PSK |
|------|-----|---------|----------|--------------|
|  0   |  1  |    2    |    3     |      4       |

---

**Note:** main branch works with latest version of FlipperZero firmware, **1.0.1** (in the moment of writting this).
If you want to have this app running with a firmware version < 1.* check the *legacy-previous-API-1* at the repo.

---

<3 & Hack the Planet!
