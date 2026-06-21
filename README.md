# co2_sensor

## Device
* ESP32
  * DOIT ESP32 DEVKIT V1
  * https://amazon.co.jp/dp/B0FT2SJH3H/
* TM1637
  * 7seg LED
  * https://amazon.co.jp/dp/B08NJD66HX/
* SCD40
  * CO2
  * https://amazon.co.jp/dp/B09Y1HRLGT/
* MOSFET
  * https://amazon.co.jp/dp/B0D6ZBKWDF/
* LED
  * HK-LED3H
  * https://amazon.co.jp/dp/B00940VQ5U/
* USB PD Trig
  * https://amazon.co.jp/dp/B0GFCXP9PZ/
* DCDC down
  * https://amazon.co.jp/dp/B07CB94KNZ/

## Wireing

| ESP32 pin | device | pin |
|-----------|--------|-----|
| GND       | TM1637 | GND |
| 3V3       | TM1637 | VCC |
| D15       | TM1637 | CLK |
| D4        | TM1637 | DIO |
| GND       | SCD40  | GND |
| 3V3       | SCD40  | VCC |
| D21       | SCD40  | SDA |
| D22       | SCD40  | SCL |
| D18       | MOSFET | TRIG/PWM |
| D19       | LED  | Cathode |

| TM1637 pin | device | pin |
|------------|--------|-----|
| GND        | ESP32  | GND |
| VCC        | ESP32  | 3V3 |
| CLK        | ESP32  | D15 |
| DIO        | ESP32  | D4  |

| SCD40 pin | device | pin |
|-----------|--------|-----|
| GND       | ESP32  | GND |
| VCC       | ESP32  | 3V3 |
| SDA       | ESP32  | D21 |
| SCL       | ESP32  | D22 |

| MOSFET pin | device | pin |
|-----------|--------|-----|
| GND       | ESP32  | GND |
| TRIG/PWM  | ESP32  | D18 |
| VIN+ | USB PD  | VBUS(+12V) |
| VIN- | USB PD  | GND |
| VOUT+ | FAN  | 2:+12V |
| VOUT- | FAN  | 1:GND |

| LED pin | device | pin |
|-----------|--------|-----|
| anode    | ESP32  | VIN via Resistor|
| Cathode  | ESP32  | D19 |

| DCDC down pin | device | pin |
|-----------|--------|-----|
| IN+ | USB PD  | VBUS(+12V) |
| GND | USB PD  | GND |
| VO+ | ESP32  | VIN(+5V) |
