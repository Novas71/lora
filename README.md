# LoRa Sensors for Home Assistant

MVP projekt pro hardware **XIAO ESP32S3 + Wio-SX1262 Kit**:
- `sensor node` firmware: periodicky odešle telemetrii přes LoRa.
- `gateway node` firmware: přijme LoRa pakety a publikuje je přímo do MQTT pro Home Assistant.
- `bridge` (Python): volitelná alternativa pro serial -> MQTT.

## Protokol

Uplink protokol je nově **obecný**:
- všechny nody používají stejný packet header
- payload tvoří seznam metrik `metric_id + value`
- různé typy node se liší jen tím, které metriky do packetu vloží

To znamená, že pro nový typ node už typicky není potřeba zavádět nový binární packet, ale jen nové `metric_id` a jejich mapování v gateway.

## Zigbee-like F1 (join + short address + interview)

Firmware nyní obsahuje první vrstvu Zigbee-like chování:
- node po startu odešle `JOIN_REQUEST`
- gateway přidělí per-node `short_addr` (16-bit, uložené v NVS gateway)
- gateway vrátí `JOIN_RESPONSE`
- node odešle `INTERVIEW_REPORT` (device type + metriky + interval)

MQTT výstupy:
- `lora2ha/node/<node_id>/join`
- `lora2ha/node/<node_id>/interview`

Ve standardním `state/status` payloadu je navíc `short_addr`.

## Zigbee-like F2 (cluster / attribute)

Druhá vrstva přidává `cluster/attribute` model:
- gateway umí poslat `read_attr` / `write_attr`
- node vrací binární packet `ATTR_REPORT` (na čtení i po úspěšném zápisu)
- kompatibilita s původním `set_param` zůstává

MQTT příklady:
- `{"cmd":"read_attr","cluster":1024,"attr":2}` (temperature)
- `{"cmd":"write_attr","cluster":61440,"attr":4097,"value":300}` (`tx_interval_sec`)

Poznámka:
- `cluster` a `attr` jsou nyní numerická ID podle [docs/payload.md](docs/payload.md).
- názvy `read_attr` / `write_attr` jsou MQTT `cmd`; `ATTR_REPORT` je název binárního packetu.
- kompletní autoritativní mapa `cluster/attribute` ID je v sekci **Attribute IDs (autorita)** v [docs/payload.md](docs/payload.md).

## Zigbee-like F3 (reporting + bind + group)

F3 je implementováno v gateway logice:
- group membership pro nody
- multicast příkazy na skupinu (`.../group/<name>/set`)
- bind pravidla `src attr -> dst write_attr` s `scale` a `offset`

Příklady:
- `{"cmd":"group_add","group":"tank"}` na `.../node/<node>/set`
- `{"cmd":"read_attr","cluster":1024,"attr":2}` na `.../group/tank/set`
- `{"cmd":"bind_attr","src_cluster":1280,"src_attr":17,"dst_node":2002,"dst_cluster":61440,"dst_attr":4097,"scale":1.0,"offset":0}`

F3 quick start (3 kroky):
1. Přidej nody do group `tank`:
	- pošli `{"cmd":"group_add","group":"tank"}` na `lora2ha/node/2001/set`
	- pošli `{"cmd":"group_add","group":"tank"}` na `lora2ha/node/2002/set`
2. Ověř group multicast:
	- pošli `{"cmd":"write_attr","cluster":61440,"attr":4097,"value":300}` na `lora2ha/group/tank/set`
3. Přidej bind pravidlo (automatická propagace reportů):
	- pošli `{"cmd":"bind_attr","src_cluster":1280,"src_attr":17,"dst_node":2002,"dst_cluster":61440,"dst_attr":4097,"scale":1.0,"offset":0}` na `lora2ha/node/2001/set`

Debug/status topicy:
- `.../node/<node_id>/groups`
- `.../node/<node_id>/bindings`
- `.../node/<node_id>/report_config`

`get_config` / `report_config`:
- pošli `{"cmd":"get_config"}` na `lora2ha/node/<node_id>/set`
- gateway okamžitě publikuje agregovaný `report_config`
- zároveň vyžádá `ATTR_TX_INTERVAL_SEC`, takže se `interval_sec` po probuzení node obnoví i z firmware

`report_config` payload obsahuje typicky:
- `interval_sec`
- `groups` + `groups_csv` + `group_count`
- `bindings` + `bindings_count`
- `device_type_name`

LoRa transport mezi node a gateway je šifrovaný a autentizovaný symetricky přes `AES-GCM` obálku.
Nastavení klíče je v [firmware/include/lora_config.h](firmware/include/lora_config.h):
- `LORA_AES_KEY_HEX` (32 hex znaků = 16 bajtů)
- hodnota musí být **stejná** na gateway i všech node
- výchozí hodnota je pouze placeholder pro vývoj, před produkcí ji změň

## Struktura

- `firmware/` PlatformIO projekt pro ESP32S3 (`sensor` a `gateway` env)
- `bridge/ha_bridge.py` MQTT bridge pro Home Assistant

## Výchozí pin mapping (Wio-SX1262 kit)

Nastaveno podle Meshtastic varianty `seeed_xiao_s3`:
- `SCK=7`, `MISO=8`, `MOSI=9`
- `CS=41`, `RST=42`, `DIO1=39`, `BUSY=40`

Pokud by u tvé revize neseděly piny, přepiš je v `firmware/include/lora_config.h`.

## Měření baterie (reálný ADC)

Oba nody teď používají skutečné ADC měření baterie (žádný placeholder):
- `SENSOR_BATTERY_ADC_PIN`, `DISTANCE_BATTERY_ADC_PIN`
- `SENSOR_BATTERY_DIVIDER_RATIO`, `DISTANCE_BATTERY_DIVIDER_RATIO`
- `BATTERY_ADC_SAMPLES`
- `SENSOR_LOW_BATTERY_MV`, `DISTANCE_LOW_BATTERY_MV`

Pro běžný odporový dělič 1:1 nech `*_DIVIDER_RATIO=2.0`.
Pokud používáš jiný dělič, uprav poměr podle HW.

## 0) První spuštění gateway (captive portal)

Gateway používá `WiFiManager`:
- když se nepřipojí na známou Wi‑Fi, spustí fallback AP `LoRaGateway-Setup`
- otevři captive portal a nastav Wi‑Fi + MQTT + volitelně statickou IP

Výchozí AP údaje (lze změnit v [firmware/include/lora_config.h](firmware/include/lora_config.h)):
- `FALLBACK_AP_SSID=LoRaGateway-Setup`
- `FALLBACK_AP_PASSWORD=loragw123`

V portálu nastavíš:
- `MQTT host`, `MQTT port`, `MQTT user/pass`, `MQTT base topic`, `MQTT client id`
- `Static IP`, `Gateway`, `Subnet mask`, `DNS` (nech prázdné pro DHCP)

Hodnoty se uloží do NVS a přežijí restart.

## 1) Flash gateway firmware

```powershell
cd firmware
pio run -e xiao_esp32s3_gateway -t upload
pio device monitor -e xiao_esp32s3_gateway
```

Po startu očekávej v monitoru: připojení Wi‑Fi/MQTT a `Gateway ready`.

## 2) Flash sensor firmware

Uprav `APP_NODE_ID` v `firmware/platformio.ini` (env `xiao_esp32s3_sensor`) pro každý uzel unikátně.

```powershell
cd firmware
pio run -e xiao_esp32s3_sensor -t upload
pio device monitor -e xiao_esp32s3_sensor
```

Node pošle packet, otevře krátké RX okno (`DOWNLINK_RX_WINDOW_MS`) pro downlink a pak jde do deep sleep.

## 2b) Flash distance node (JSN-SR04T, vodní nádrž)

Pro nový node použij env:

```powershell
cd firmware
pio run -e xiao_esp32s3_distance_node -t upload
pio device monitor -e xiao_esp32s3_distance_node
```

Výchozí perioda odeslání je 5 minut (`DISTANCE_TX_INTERVAL_SEC=300`).

Node je připravený jako senzor objemu vody v nádrži.
Konfigurace v [firmware/include/lora_config.h](firmware/include/lora_config.h):
- `TANK_AREA_M2` – plocha nádrže v m²
- `TANK_DISTANCE_MIN_MM` – minimální vzdálenost hladiny od senzoru (typicky plná nádrž)
- `TANK_DISTANCE_MAX_MM` – maximální vzdálenost hladiny od senzoru (typicky prázdná nádrž)

Výpočet v node:
- `hladina_mm = TANK_DISTANCE_MAX_MM - naměřená_vzdálenost_mm` (saturace do rozsahu min/max)
- `objem_l = TANK_AREA_M2 * hladina_mm`

Doporučené piny v [firmware/include/lora_config.h](firmware/include/lora_config.h):
- `JSN_TRIG_PIN`
- `JSN_ECHO_PIN`
- `JSN_POWER_PIN` (volitelné, přes tranzistorové spínání napájení)

Poznámka k HW:
- `JSN-SR04T` často vrací `ECHO` na 5V, proto použij level shifter / dělič napětí na vstup ESP32.

## 3) Home Assistant

V HA musí běžet MQTT broker (typicky Mosquitto addon).
Po prvním paketu se automaticky vytvoří entity přes MQTT Discovery:
- `temperature`, `humidity`, `pressure`, `battery`, `rssi`, `snr`
- `TX interval`, `Groups`, `Bindings` (z `report_config`)
- `Last seen age`, `Node stale` (z `liveness`)

U distance node se vytvoří a plní zejména:
- `distance_cm` (aktuální vzdálenost od senzoru)
- `level_cm` (výška hladiny)
- `water_l` (vypočtený objem vody)
- `battery`, `rssi`, `snr`
- `low_battery` (binary sensor)

Topicy:
- `lora2ha/node/<node_id>/state`
- `lora2ha/node/<node_id>/status`
- `lora2ha/node/<node_id>/availability`
- `lora2ha/node/<node_id>/report_config`
- `lora2ha/node/<node_id>/liveness`
- `lora2ha/gateway/status`
- `lora2ha/node/<node_id>/ack`

`liveness` payload obsahuje typicky:
- `last_seen_age_sec`
- `timeout_sec`
- `stale` / `online`
- `source`
- `event` (`seen`, `tick`, `became_stale`, `became_online`)

Gateway odvozuje timeout automaticky z `interval_sec`:
- krátké intervaly: přibližně `4x`
- delší intervaly: přibližně `3x`
- bez známého intervalu fallback na `1800 s`

## 4) Downlink příkazy z Home Assistantu

Gateway poslouchá topic:
- `lora2ha/node/<node_id>/set`

Podporované JSON příkazy:
- `{"cmd":"ping"}`
- `{"cmd":"reboot"}`
- `{"cmd":"get_config"}`
- `{"cmd":"set_param","param":"tx_interval_sec","value":300}`
- `{"cmd":"enter_ota","sec":300}`
- `{"cmd":"set_param","param":"tank_area_m2","value":1.25}`
- `{"cmd":"set_param","param":"tank_distance_min_mm","value":250}`
- `{"cmd":"set_param","param":"tank_distance_max_mm","value":1900}`

Legacy aliasy (`set_interval`, `set_tank_*`) jsou stále akceptované kvůli zpětné kompatibilitě.

Poznámka:
- konfigurace nádrže se uloží do NVS v distance node a zůstane zachovaná po restartu/deep sleep.

Node vrací ACK do:
- `lora2ha/node/<node_id>/ack`

ACK payload obsahuje:
- `acked_fcnt`, `op`, `param`, `status`, `interval_sec`, `rssi`, `snr`

`status` kódy:
- `0` OK
- `1` unsupported command
- `2` invalid value
- `3` replay detected / stale target frame

## 4b) Replay protection

Je doplněna základní replay ochrana nad existujícími čítači:
- gateway přijímá telemetry uplink jen pokud má `frame_counter` přísně větší než poslední přijatý pro daný node
- node přijme downlink / `ATTR_CMD` jen pokud `target_frame_counter` přesně odpovídá právě odeslanému uplinku

Důsledek:
- staré přehrané telemetry pakety gateway zahodí
- staré přehrané control pakety node odmítne s `ACK status = 3`

## 5) OTA podpora

### Gateway OTA (Wi‑Fi)

Gateway běží s `ArduinoOTA` automaticky po připojení na Wi‑Fi.

Konfigurace v [firmware/include/lora_config.h](firmware/include/lora_config.h):
- `OTA_HOSTNAME_GATEWAY`
- `OTA_PASSWORD` (volitelně)

### Gateway OTA přes web interface

Gateway zároveň hostuje web OTA stránku:
- `http://<gateway_ip>/`

Status endpoint (JSON):
- `http://<gateway_ip>/status`

Funkce:
- upload `.bin` firmware souboru pro `xiao_esp32s3_gateway`
- po úspěšném uploadu automatický restart gateway
- JSON diagnostika (`wifi_connected`, `mqtt_connected`, `uptime_sec`, `free_heap`, ...)

Autentizace:
- pokud je `OTA_PASSWORD` prázdné, web je bez auth
- pokud je `OTA_PASSWORD` nastavené, web používá Basic Auth
	- user: `admin`
	- password: hodnota `OTA_PASSWORD`

### Node OTA (na vyžádání)

Node je normálně v deep sleep. OTA režim se spouští downlinkem:
- `{"cmd":"enter_ota","sec":300}`

Po přijetí příkazu node:
- odešle ACK
- připojí se na Wi‑Fi (`WIFI_SSID`/`WIFI_PASSWORD`)
- otevře OTA okno na `sec` sekund

Konfigurace v [firmware/include/lora_config.h](firmware/include/lora_config.h):
- `OTA_HOSTNAME_SENSOR`
- `OTA_PASSWORD`
- `OTA_DEFAULT_WINDOW_SEC`

### OTA z PlatformIO

Gateway (síťový upload):

```powershell
cd firmware
pio run -e xiao_esp32s3_gateway -t upload --upload-port lora-gateway.local
```

Node (během aktivního OTA okna):

```powershell
cd firmware
pio run -e xiao_esp32s3_sensor -t upload --upload-port lora-sensor.local
```

### Hotový HA package (UI tlačítka)

V repu je připravený package se helpery + skripty:
- [docs/homeassistant/lora_controls_package.yaml](docs/homeassistant/lora_controls_package.yaml)
- Slot helpery pro overview dashboard:
- [docs/homeassistant/lora_overview_slots_package.yaml](docs/homeassistant/lora_overview_slots_package.yaml)
- Gateway status package (REST):
- [docs/homeassistant/lora_gateway_status_package.yaml](docs/homeassistant/lora_gateway_status_package.yaml)

Co obsahuje:
- `input_text.lora_target_node_id`
- `input_number.lora_target_interval_sec`
- skripty `script.lora_ping_node`, `script.lora_reboot_node`, `script.lora_set_interval_node`, `script.lora_get_config_node`
- ACK notifikaci přes `persistent_notification`
- low-battery notifikaci přes `persistent_notification` (trigger z `lora2ha/node/+/state`)
- liveness notifikaci při `became_stale` a její zrušení při `became_online`

Zapnutí package v Home Assistantu (`configuration.yaml`):

```yaml
homeassistant:
	packages: !include_dir_named packages
```

Pak zkopíruj soubor do HA:
- `<config>/packages/lora_controls_package.yaml`
- `<config>/packages/lora_overview_slots_package.yaml`

Pro monitoring gateway status endpointu `/status` zkopíruj také:
- `<config>/packages/lora_gateway_status_package.yaml`

Pak v HA nastav `input_text.lora_gateway_ip` na IP adresu gateway.

A udělej `Restart Home Assistant`.

### Lovelace karta (volitelné)

Připravená karta je v:
- [docs/homeassistant/lovelace_lora_controls.yaml](docs/homeassistant/lovelace_lora_controls.yaml)
- Multi-node varianta (3 rychlé panely):
- [docs/homeassistant/lovelace_lora_controls_multi.yaml](docs/homeassistant/lovelace_lora_controls_multi.yaml)
- Přehledový fleet dashboard:
- [docs/homeassistant/lovelace_lora_overview.yaml](docs/homeassistant/lovelace_lora_overview.yaml)

Použití v dashboardu:
- otevři dashboard -> `Edit dashboard` -> `Add card` -> `Manual`
- vlož obsah z [docs/homeassistant/lovelace_lora_controls.yaml](docs/homeassistant/lovelace_lora_controls.yaml)

Pro multi-node variantu vlož místo toho obsah z
- [docs/homeassistant/lovelace_lora_controls_multi.yaml](docs/homeassistant/lovelace_lora_controls_multi.yaml)

Pro provozní monitoring celé flotily můžeš vložit obsah z
- [docs/homeassistant/lovelace_lora_overview.yaml](docs/homeassistant/lovelace_lora_overview.yaml)

V overview kartě změň hodnoty `LoRa Overview Node 1/2/3` a dashboard se přepne na zvolené node bez editace YAML.

Poznámka:
- v kartách je mini panel Tank Metrics s příkladovými entitami `sensor.lora_2001_distance_cm`, `sensor.lora_2001_level_cm`, `sensor.lora_2001_water_l`
- pokud má Home Assistant entity pojmenované jinak, uprav tyto 3 entity přímo v Lovelace YAML

## 6) Volitelně: Python bridge

Na Raspberry Pi (nebo jiném stroji s přístupem na MQTT):

```bash
cd bridge
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python ha_bridge.py --serial-port /dev/ttyUSB0 --mqtt-host 127.0.0.1 --mqtt-port 1883
```

Windows příklad:

```powershell
cd bridge
py -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
python ha_bridge.py --serial-port COM7 --mqtt-host 192.168.1.20 --mqtt-port 1883
```

## LoRa parametry (EU868)

V `firmware/include/lora_config.h`:
- `LORA_FREQUENCY_MHZ=868.1`
- `BW=125 kHz`, `SF=9`, `CR=4/5` (RadioLib `coding_rate=7`)

## Poznámky k produkci

- Tento MVP používá `CRC16` proti poškození rámce, ne šifrování.
- Pro produkční síť přidej per-node klíče a key rotation.
- Dodržuj regionální duty-cycle limity.
