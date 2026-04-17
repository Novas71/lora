import argparse
import json
import sys
import time
from datetime import datetime, timezone

import paho.mqtt.client as mqtt
import serial

BRIDGE_STATUS_TOPIC = "lora2ha/bridge/status"


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def discovery_payload(node_id: int, key: str, name: str, unit: str | None, device_class: str | None, state_class: str | None):
    payload = {
        "name": name,
        "unique_id": f"lora_{node_id}_{key}",
        "state_topic": f"lora2ha/node/{node_id}/state",
        "availability_topic": f"lora2ha/node/{node_id}/availability",
        "value_template": f"{{{{ value_json.{key} }}}}",
        "device": {
            "identifiers": [f"lora_{node_id}"],
            "name": f"LoRa Node {node_id}",
            "manufacturer": "Seeed Studio",
            "model": "XIAO ESP32S3 + Wio-SX1262",
        },
    }
    if unit:
        payload["unit_of_measurement"] = unit
    if device_class:
        payload["device_class"] = device_class
    if state_class:
        payload["state_class"] = state_class
    return payload


def publish_discovery(client: mqtt.Client, node_id: int):
    sensors = [
        ("temperature", "Temperature", "°C", "temperature", "measurement"),
        ("humidity", "Humidity", "%", "humidity", "measurement"),
        ("pressure", "Pressure", "hPa", "pressure", "measurement"),
        ("battery", "Battery", "V", "voltage", "measurement"),
        ("rssi", "RSSI", "dBm", "signal_strength", "measurement"),
        ("snr", "SNR", "dB", None, "measurement"),
    ]

    for key, name, unit, device_class, state_class in sensors:
        topic = f"homeassistant/sensor/lora_{node_id}_{key}/config"
        payload = discovery_payload(node_id, key, name, unit, device_class, state_class)
        client.publish(topic, json.dumps(payload), retain=True)


def parse_args():
    parser = argparse.ArgumentParser(description="LoRa serial to Home Assistant MQTT bridge")
    parser.add_argument("--serial-port", required=True, help="Serial port with gateway node, e.g. COM7")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--mqtt-host", default="127.0.0.1")
    parser.add_argument("--mqtt-port", type=int, default=1883)
    parser.add_argument("--mqtt-user", default=None)
    parser.add_argument("--mqtt-pass", default=None)
    parser.add_argument("--base-topic", default="lora2ha")
    return parser.parse_args()


def main():
    args = parse_args()

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="lora2ha_bridge", clean_session=True)
    if args.mqtt_user:
        client.username_pw_set(args.mqtt_user, args.mqtt_pass)

    client.will_set(BRIDGE_STATUS_TOPIC, payload="offline", qos=1, retain=True)
    client.connect(args.mqtt_host, args.mqtt_port, keepalive=30)
    client.loop_start()
    client.publish(BRIDGE_STATUS_TOPIC, payload="online", qos=1, retain=True)

    discovered_nodes: set[int] = set()

    try:
        with serial.Serial(args.serial_port, args.baud, timeout=1) as ser:
            while True:
                line = ser.readline().decode("utf-8", errors="ignore").strip()
                if not line:
                    continue
                if not line.startswith("{"):
                    continue

                try:
                    payload = json.loads(line)
                except json.JSONDecodeError:
                    continue

                if "error" in payload:
                    print(f"Gateway error: {payload}", file=sys.stderr)
                    continue

                node_id = payload.get("node_id")
                if node_id is None:
                    continue

                if node_id not in discovered_nodes:
                    publish_discovery(client, node_id)
                    discovered_nodes.add(node_id)

                state_topic = f"{args.base_topic}/node/{node_id}/state"
                availability_topic = f"{args.base_topic}/node/{node_id}/availability"
                status_topic = f"{args.base_topic}/node/{node_id}/status"

                state_payload = {
                    "temperature": payload.get("temperature"),
                    "humidity": payload.get("humidity"),
                    "pressure": payload.get("pressure"),
                    "battery": payload.get("battery"),
                    "rssi": payload.get("rssi"),
                    "snr": payload.get("snr"),
                    "fcnt": payload.get("fcnt"),
                    "flags": payload.get("flags", 0),
                    "last_seen": now_iso(),
                }
                client.publish(state_topic, json.dumps(state_payload), retain=False)
                client.publish(status_topic, json.dumps(payload), retain=False)
                client.publish(availability_topic, "online", retain=True)

    except KeyboardInterrupt:
        pass
    finally:
        client.publish(BRIDGE_STATUS_TOPIC, payload="offline", qos=1, retain=True)
        client.loop_stop()
        client.disconnect()


if __name__ == "__main__":
    main()
