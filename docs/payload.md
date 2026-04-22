# Payload specifikace (V1)

## Generic Metrics Uplink

Uplink je nyní obecný a všechny nody používají stejný packet:

### `MetricsPacketHeaderV1`

- `proto_ver` (u8)
- `msg_type` (u8, aktuálně `0x01`)
- `node_id` (u32)
- `frame_counter` (u32)
- `unix_time` (u32)
- `metric_count` (u8)
- `flags` (u8)

Za headerem následuje `metric_count` záznamů typu `MetricRecordV1`:

### `MetricRecordV1`

- `metric_id` (u8)
- `value` (i32)

Na konci packetu je:

- `crc16` (u16, CCITT)

Celková délka packetu je:

$$
	ext{size} = 16 + (5 \times \text{metric\_count}) + 2
$$

Maximálně je aktuálně podporováno `8` metrik v jednom uplinku.

### Metric IDs

- `0x01` `METRIC_BATTERY_MV`
- `0x02` `METRIC_TEMPERATURE_C_X100`
- `0x03` `METRIC_HUMIDITY_RH_X100`
- `0x04` `METRIC_PRESSURE_HPA_X10`
- `0x10` `METRIC_DISTANCE_MM`
- `0x11` `METRIC_LEVEL_MM`
- `0x12` `METRIC_WATER_L_X10`

Tím pádem už není potřeba mít pro každý typ node vlastní uplink packet – node jen pošle jinou kombinaci metrik.

## DownlinkPacketV1

Binární struktura (`DownlinkPacketV1`):

- `proto_ver` (u8)
- `msg_type` (u8, `0x10`)
- `node_id` (u32)
- `target_frame_counter` (u32)
- `operation` (u8)
- `parameter_id` (u16)
- `value_i32` (i32)
- `crc16` (u16, CCITT)

Celkem: 19 B.

### Downlink operations

- `0x01` `DL_OP_PING` (bez parametru)
- `0x02` `DL_OP_SET_PARAM` (`parameter_id` + `value_i32`)
- `0x03` `DL_OP_REBOOT` (bez parametru)
- `0x04` `DL_OP_ENTER_OTA` (`value_i32 = ota_window_sec`)

### Parameter IDs

- `0x0001` `PARAM_TX_INTERVAL_SEC`
- `0x0101` `PARAM_TANK_AREA_M2_X1000`
- `0x0102` `PARAM_TANK_DISTANCE_MIN_MM`
- `0x0103` `PARAM_TANK_DISTANCE_MAX_MM`

### Parameter schema keys (MQTT `set_param`)

- `tx_interval_sec`
- `tank_area_m2`
- `tank_distance_min_mm`
- `tank_distance_max_mm`

## AckPacketV1

Binární struktura (`AckPacketV1`):

- `proto_ver` (u8)
- `msg_type` (u8, `0x11`)
- `node_id` (u32)
- `acked_frame_counter` (u32)
- `acked_operation` (u8)
- `acked_parameter_id` (u16)
- `status` (u8)
- `current_interval_sec` (u32)
- `crc16` (u16, CCITT)

Celkem: 20 B.

### ACK status

- `0` `ACK_OK`
- `1` `ACK_UNSUPPORTED_CMD`
- `2` `ACK_INVALID_VALUE`

### Uplink flags

- bit `0`: stavový flag node, aktuálně např. low battery nebo chyba měření dle implementace konkrétního node

### Výpočty pro nádrž

- `level_mm = TANK_DISTANCE_MAX_MM - distance_mm` (saturace na `0 .. (TANK_DISTANCE_MAX_MM - TANK_DISTANCE_MIN_MM)`)
- `water_liters_x10 = water_liters * 10`
- `water_liters = water_liters_x10 / 10.0`

## Hodnoty

- `temp_c_x100`: `2356` => `23.56 °C`
- `rh_x100`: `4512` => `45.12 %`
- `pressure_pa_div10`: `10132` => `1013.2 hPa`

## Flags

- bit `0`: low battery

## Kompatibilita

- Brána očekává `proto_ver = 1`.
- Neznámá verze je odmítnuta (`unsupported_proto`).
