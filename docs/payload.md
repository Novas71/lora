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

## Join / Addressing (F1)

Nové packety pro Zigbee-like onboarding:

### `JoinRequestV1`

- `proto_ver` (u8, `1`)
- `msg_type` (u8, `0x20`)
- `node_id` (u32)
- `device_type` (u8)
- `capabilities` (u32 bitmap)
- `fw_version` (u16)
- `crc16` (u16)

### `JoinResponseV1`

- `proto_ver` (u8, `1`)
- `msg_type` (u8, `0x21`)
- `node_id` (u32)
- `short_addr` (u16)
- `network_id` (u16)
- `status` (u8)
- `crc16` (u16)

### `InterviewReportV1`

- `proto_ver` (u8, `1`)
- `msg_type` (u8, `0x22`)
- `node_id` (u32)
- `short_addr` (u16)
- `device_type` (u8)
- `metric_bitmap` (u32)
- `current_interval_sec` (u32)
- `crc16` (u16)

Gateway publikuje onboarding data do MQTT:
- `.../node/<node_id>/join`
- `.../node/<node_id>/interview`

## Cluster / Attribute (F2)

Terminologie:
- `read_attr` / `write_attr` = MQTT `cmd` názvy používané gateway API.
- `ATTR_REPORT` = binární uplink packet (`AttrReportPacketV1`).

### `AttrCommandPacketV1`

- `proto_ver` (u8, `1`)
- `msg_type` (u8, `0x30`)
- `node_id` (u32)
- `target_frame_counter` (u32)
- `command_type` (u8)
- `cluster_id` (u16)
- `attribute_id` (u16)
- `value_i32` (i32)
- `crc16` (u16)

`command_type`:
- `0x01` `ATTR_CMD_READ`
- `0x02` `ATTR_CMD_WRITE`

### `AttrReportPacketV1`

- `proto_ver` (u8, `1`)
- `msg_type` (u8, `0x31`)
- `node_id` (u32)
- `short_addr` (u16)
- `cluster_id` (u16)
- `attribute_id` (u16)
- `value_i32` (i32)
- `flags` (u8)
- `crc16` (u16)

### Cluster IDs

- `0x0001` `CLUSTER_POWER`
- `0x0400` `CLUSTER_ENVIRONMENT`
- `0x0500` `CLUSTER_WATER_TANK`
- `0xF000` `CLUSTER_SYSTEM`

### Attribute IDs (autorita)

- `CLUSTER_POWER` (`0x0001` / `1`)
	- `ATTR_BATTERY_MV` `0x0001` / `1`
- `CLUSTER_ENVIRONMENT` (`0x0400` / `1024`)
	- `ATTR_TEMPERATURE_C_X100` `0x0002` / `2`
	- `ATTR_HUMIDITY_RH_X100` `0x0003` / `3`
	- `ATTR_PRESSURE_HPA_X10` `0x0004` / `4`
- `CLUSTER_WATER_TANK` (`0x0500` / `1280`)
	- `ATTR_DISTANCE_MM` `0x0010` / `16`
	- `ATTR_LEVEL_MM` `0x0011` / `17`
	- `ATTR_WATER_L_X10` `0x0012` / `18`
	- `ATTR_TANK_AREA_M2_X1000` `0x1101` / `4353`
	- `ATTR_TANK_DISTANCE_MIN_MM` `0x1102` / `4354`
	- `ATTR_TANK_DISTANCE_MAX_MM` `0x1103` / `4355`
- `CLUSTER_SYSTEM` (`0xF000` / `61440`)
	- `ATTR_TX_INTERVAL_SEC` `0x1001` / `4097`

### MQTT ovládání (gateway -> node)

Topic:
- `lora2ha/node/<node_id>/set`

JSON:
- `{"cmd":"read_attr","cluster":1024,"attr":2}`
- `{"cmd":"write_attr","cluster":61440,"attr":4097,"value":600}`

Node odpovídá:
- `ATTR_REPORT` uplinkem (mapovaný do `state/status`)
- a standardním `ACK` (`op` = command_type, `param` = attribute_id)

## F3: Reporting + Bind + Group

F3 je implementované v gateway vrstvě nad F2:
- `ATTR_REPORT` a telemetry metriky mohou spouštět bind pravidla.
- group multicast příkazy se posílají do `lora2ha/group/<group>/set`.

### Group management (topic `.../node/<node_id>/set`)

- `{"cmd":"group_add","group":"tank"}`
- `{"cmd":"group_remove","group":"tank"}`
- `{"cmd":"group_clear"}`
- `{"cmd":"group_list"}`

Gateway publikuje membership do:
- `.../node/<node_id>/groups`

### Group multicast commands (topic `.../group/<group>/set`)

Podporuje stejné payloady jako node-level `.../node/<node_id>/set`, např.:
- `{"cmd":"read_attr","cluster":1024,"attr":2}`
- `{"cmd":"write_attr","cluster":61440,"attr":4097,"value":300}`

### Attribute binding (topic `.../node/<src_node>/set`)

- `{"cmd":"bind_attr","src_cluster":1280,"src_attr":17,"dst_node":2002,"dst_cluster":61440,"dst_attr":4097,"scale":1.0,"offset":0}`
- `{"cmd":"unbind_attr","src_cluster":1280,"src_attr":17,"dst_node":2002,"dst_cluster":61440,"dst_attr":4097}`
- `{"cmd":"bind_list"}`

Gateway publikuje bindings do:
- `.../node/<node_id>/bindings`

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

- bit `0`: `FLAG_LOW_BATTERY`
- bit `1`: `FLAG_MEASUREMENT_FAIL`

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
- bit `1`: measurement failed

## Kompatibilita

- Brána očekává `proto_ver = 1`.
- Neznámá verze je odmítnuta (`unsupported_proto`).
