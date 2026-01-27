# co2_sensor

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
