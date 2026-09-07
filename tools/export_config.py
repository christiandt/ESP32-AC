#!/usr/bin/env python3
"""Build the ESP32-AC provisioning blob from the config the Python CLIs already cached.

`mpsac setup` fetches the Midea V3 token/key from the Midea cloud once and
caches them; `elux` stores the Electrolux module's IP. This reads both and
emits a single JSON document the firmware accepts at POST /api/config, so the
credentials never have to be retyped and the ESP32 never has to touch the cloud.

  # print the blob
  python3 tools/export_config.py --ssid HomeWiFi --psk hunter2

  # also read live capabilities and setpoint limits off the Midea unit
  python3 tools/export_config.py --ssid HomeWiFi --psk hunter2 --probe

  # send it straight to a device in provisioning mode
  python3 tools/export_config.py --ssid HomeWiFi --psk hunter2 \
      --post http://192.168.4.1/api/config

The output contains secrets. Don't paste it into a bug report.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, Dict, Optional

# Must match the Feature enum in src/ac_types.h.
FEATURES = [
    "eco",
    "ieco",
    "boost",
    "sleep",
    "out_silent",
    "ion",
    "beep",
    "freeze",
    "follow_me",
    "led",
    "self_clean",
]

# Feature name -> the msmart capability attribute that gates it. None means
# msmart exposes no capability check, so we assume it is available (the same
# assumption mpsac's TOGGLES table makes).
MIDEA_CAP_ATTRS = {
    "eco": "supports_eco",
    "ieco": "supports_ieco",
    "boost": "supports_turbo",
    "sleep": None,
    "out_silent": "supports_out_silent",
    "ion": "supports_purifier",
    "beep": None,
    "freeze": "supports_freeze_protection",
    "follow_me": None,
    "led": "supports_display_control",
    "self_clean": "supports_self_clean",
}


def mpsac_config_path() -> Path:
    """Mirror of mpsac.config.config_path(), without importing mpsac."""
    override = os.environ.get("MPSAC_CONFIG_DIR")
    if override:
        return Path(override) / "config.json"
    xdg = os.environ.get("XDG_CONFIG_HOME")
    base = Path(xdg) if xdg else Path.home() / ".config"
    return base / "mpsac" / "config.json"


def electrolux_config_path() -> Path:
    return Path(os.path.expanduser("~/.electrolux_ac_config.json"))


def read_json(path: Path) -> Optional[Dict[str, Any]]:
    if not path.exists():
        return None
    try:
        return json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        print(f"warning: could not read {path}: {exc}", file=sys.stderr)
        return None


def probe_midea(cfg: Dict[str, Any]) -> Dict[str, Any]:
    """Connect to the unit and read its real capabilities and setpoint limits.

    Optional: without it we fall back to a conservative capability set. Requires
    msmart-ng and the unit being reachable.
    """
    try:
        import asyncio

        from msmart.device import AirConditioner as AC
    except ImportError:
        print("warning: --probe needs msmart-ng (pip install msmart-ng); skipping",
              file=sys.stderr)
        return {}

    async def run() -> Dict[str, Any]:
        dev = AC(ip=cfg["ip"], device_id=int(cfg["device_id"]), port=int(cfg.get("port", 6444)))
        if cfg.get("token") and cfg.get("key"):
            await dev.authenticate(cfg["token"], cfg["key"])
        await dev.get_capabilities()
        await dev.refresh()

        caps = []
        for name, attr in MIDEA_CAP_ATTRS.items():
            if attr is None or getattr(dev, attr, False):
                caps.append(name)
        return {
            "caps": caps,
            "min_target": float(dev.min_target_temperature),
            "max_target": float(dev.max_target_temperature),
        }

    try:
        return asyncio.run(run())
    except Exception as exc:  # noqa: BLE001 - probing is best-effort
        print(f"warning: probe failed ({exc}); falling back to defaults", file=sys.stderr)
        return {}


def build(args: argparse.Namespace) -> Dict[str, Any]:
    blob: Dict[str, Any] = {}

    if args.ssid:
        blob["wifi"] = {"ssid": args.ssid, "psk": args.psk or ""}
    if args.bearer:
        blob["api"] = {"bearer": args.bearer}

    midea = read_json(mpsac_config_path())
    if midea:
        entry: Dict[str, Any] = {
            "enabled": True,
            # Not midea["name"]: that is the module's own network name
            # ("net_ac_0150"), which is no use as a display name and would
            # come back every time the blob is regenerated.
            "name": args.midea_name,
            "ip": midea["ip"],
            "device_id": int(midea["device_id"]),
            "port": int(midea.get("port", 6444)),
            "token": midea.get("token") or "",
            "key": midea.get("key") or "",
            # Conservative default: everything msmart has no capability check
            # for, plus the toggles this unit family almost always has.
            "caps": ["eco", "boost", "sleep", "beep", "follow_me", "led"],
            "min_target": 16.0,
            "max_target": 30.0,
        }
        if args.probe:
            entry.update(probe_midea(midea))
        blob["midea"] = entry
    else:
        print(f"warning: no mpsac config at {mpsac_config_path()} — run `mpsac setup`",
              file=sys.stderr)

    elux = read_json(electrolux_config_path())
    if elux and elux.get("ip_address"):
        blob["electrolux"] = {
            "enabled": True,
            "name": args.electrolux_name,
            "ip": elux["ip_address"],
        }
    else:
        print(f"warning: no Electrolux config at {electrolux_config_path()} — run `elux status`",
              file=sys.stderr)

    return blob


def post(url: str, blob: Dict[str, Any], bearer: Optional[str]) -> int:
    data = json.dumps(blob).encode()
    req = urllib.request.Request(url, data=data, method="POST")
    req.add_header("Content-Type", "application/json")
    if bearer:
        req.add_header("Authorization", f"Bearer {bearer}")
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            print(resp.read().decode())
        return 0
    except urllib.error.HTTPError as exc:
        print(f"error: {exc.code} {exc.reason}\n{exc.read().decode()}", file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"error: could not reach {url}: {exc}", file=sys.stderr)
        return 1


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--ssid", help="WiFi SSID for the ESP32 to join")
    p.add_argument("--psk", help="WiFi password")
    p.add_argument("--bearer", help="Set an explicit REST bearer token (default: keep the "
                                    "device's generated one)")
    p.add_argument("--midea-name", default="Midea Porta Split",
                   help="Display name for the Midea unit")
    p.add_argument("--electrolux-name", default="Electrolux", help="Display name for the unit")
    p.add_argument("--probe", action="store_true",
                   help="Connect to the Midea unit to read real capabilities and temp limits")
    p.add_argument("--post", metavar="URL",
                   help="POST the blob to the device, e.g. http://192.168.4.1/api/config")
    args = p.parse_args()

    blob = build(args)
    if not blob.get("midea") and not blob.get("electrolux"):
        print("error: found no AC config to export", file=sys.stderr)
        return 1

    if args.post:
        return post(args.post, blob, args.bearer)

    print(json.dumps(blob, indent=2))
    print("\nThis contains WiFi and Midea credentials — handle it like a password file.",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
