Import("env")

import os
import re

PROJECT_DIR = env["PROJECT_DIR"]
ENV_PATH = os.path.join(PROJECT_DIR, ".env")
EXAMPLE_PATH = os.path.join(PROJECT_DIR, ".env.example")


def c_string_escape(s: str) -> str:
    out = []
    for ch in s:
        o = ord(ch)
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\t":
            out.append("\\t")
        elif 32 <= o <= 126:
            out.append(ch)
        else:
            out.append(f"\\{o:03o}")
    return "".join(out)


def parse_env_file(path: str) -> dict:
    out = {}
    if not os.path.isfile(path):
        return out
    with open(path, "r", encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$", line)
            if not m:
                continue
            key, val = m.group(1), m.group(2).strip()
            if len(val) >= 2 and val[0] == val[-1] and val[0] in "\"'":
                val = val[1:-1]
            out[key] = val
    return out


def define_string(name: str, value: str):
    return (name, f'\\"{c_string_escape(value)}\\"')


def define_int(name: str, value: str, default: int):
    try:
        return (name, int(value or str(default)))
    except ValueError:
        return (name, default)


def main():
    data = parse_env_file(ENV_PATH)
    if not data:
        data = parse_env_file(EXAMPLE_PATH)
        if data:
            print("load_env_config: .env missing, using .env.example")
        else:
            print("WARN load_env_config: neither .env nor .env.example exists; using empty defaults")

    def get(key: str, default: str = "") -> str:
        return data.get(key, default)

    cppdefines = [
        define_string("WIFI_SSID", get("WIFI_SSID", "")),
        define_string("WIFI_PASSWORD", get("WIFI_PASSWORD", "")),
        define_string("MQTT_HOST", get("MQTT_HOST", "127.0.0.1")),
        define_int("MQTT_PORT", get("MQTT_PORT", "1883"), 1883),
        define_string("MQTT_USER", get("MQTT_USER", "")),
        define_string("MQTT_PASSWORD", get("MQTT_PASSWORD", "")),
        define_string("MQTT_TOPIC_NOTIFY", get("MQTT_TOPIC_NOTIFY", "notifymatrix/notify")),
        define_string("IDFM_API_KEY", get("IDFM_API_KEY", "")),
        define_string("IDFM_MONITORING_REF", get("IDFM_MONITORING_REF", "")),
        define_string("IDFM_LINE_REF", get("IDFM_LINE_REF", "")),
        define_string("IDFM_LINE_LABEL", get("IDFM_LINE_LABEL", "2225")),
        define_string("IDFM_LINE_CODE", get("IDFM_LINE_CODE", "")),
        define_string("IDFM_DESTINATION_FILTER", get("IDFM_DESTINATION_FILTER", "")),
        ("IDFM_PREFER_THEORETICAL", int(get("IDFM_PREFER_THEORETICAL", "0") or "0")),
    ]

    try:
        slot_count = int(get("IDFM_SLOT_COUNT", "0") or "0")
    except ValueError:
        slot_count = 0
    if slot_count < 0:
        slot_count = 0
    if slot_count > 16:
        raise ValueError("IDFM_SLOT_COUNT is limited to 16 slots")
    cppdefines.append(("IDFM_SLOT_COUNT", slot_count))
    for i in range(slot_count):
        prefix = f"IDFM_SLOT_{i}_"
        cppdefines.extend(
            [
                define_string(f"{prefix}MONITORING_REF", get(f"{prefix}MONITORING_REF", "")),
                define_string(f"{prefix}LINE_REF", get(f"{prefix}LINE_REF", "")),
                define_string(f"{prefix}LINE_CODE", get(f"{prefix}LINE_CODE", "")),
                define_string(f"{prefix}LABEL", get(f"{prefix}LABEL", "")),
                (f"{prefix}PREFER_THEORETICAL", int(get(f"{prefix}PREFER_THEORETICAL", "0") or "0")),
            ]
        )

    env.Append(CPPDEFINES=cppdefines)


main()
