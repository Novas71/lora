# Payload specifikace (V1)

Binární struktura (`TelemetryPacketV1`):

- `proto_ver` (u8)
- `msg_type` (u8)
- `node_id` (u32)
- `frame_counter` (u32)
- `unix_time` (u32)
- `battery_mV` (u16)
- `temp_c_x100` (i16)
- `rh_x100` (u16)
- `pressure_pa_div10` (u16)
- `flags` (u8)
- `crc16` (u16, CCITT)

Celkem: 24 B.

## DownlinkPacketV1

Binární struktura (`DownlinkPacketV1`):

- `proto_ver` (u8)
- `msg_type` (u8, `0x10`)
- `node_id` (u32)
- `target_frame_counter` (u32)
- `cmd` (u8)
- `reserved` (u8)
- `value_u32` (u32)
- `crc16` (u16, CCITT)

Celkem: 18 B.

### Downlink commands

- `0x01` `CMD_PING`
- `0x02` `CMD_SET_INTERVAL_SEC` (`value_u32 = sec`)
- `0x03` `CMD_REBOOT`
- `0x04` `CMD_ENTER_OTA` (`value_u32 = ota_window_sec`)
- `0x20` `CMD_SET_TANK_AREA_M2_X1000` (`value_u32 = area_m2 * 1000`)
- `0x21` `CMD_SET_TANK_DISTANCE_MIN_MM` (`value_u32 = min_mm`)
- `0x22` `CMD_SET_TANK_DISTANCE_MAX_MM` (`value_u32 = max_mm`)

## AckPacketV1

Binární struktura (`AckPacketV1`):

- `proto_ver` (u8)
- `msg_type` (u8, `0x11`)
- `node_id` (u32)
- `acked_frame_counter` (u32)
- `acked_cmd` (u8)
- `status` (u8)
- `current_interval_sec` (u32)
- `crc16` (u16, CCITT)

Celkem: 18 B.

### ACK status

- `0` `ACK_OK`
- `1` `ACK_UNSUPPORTED_CMD`
- `2` `ACK_INVALID_VALUE`

## DistancePacketV1

Binární struktura (`DistancePacketV1`):

- `proto_ver` (u8)
- `msg_type` (u8, `0x04`)
- `node_id` (u32)
- `frame_counter` (u32)
- `unix_time` (u32)
- `battery_mV` (u16)
- `distance_mm` (u16)
- `level_mm` (u16)
- `water_liters_x10` (u32)
- `flags` (u8)
- `crc16` (u16, CCITT)

Celkem: 27 B.

### Distance flags

- bit `0`: měření vzdálenosti selhalo (timeout/no echo)

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
