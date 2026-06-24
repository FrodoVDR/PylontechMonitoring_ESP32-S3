#!/usr/bin/env python3
"""
Checkmk local check for PylontechMonitoring /api/monitoring.

Usage examples:
  ./checkmk_pylontech_monitoring.py --host 192.168.88.22
  ./checkmk_pylontech_monitoring.py --host 192.168.88.22 --port 80 --timeout 3

Output: Checkmk local check lines.
"""

from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.request
from typing import Any, Dict, Optional, Tuple


def fetch_json(host: str, port: int, timeout: float) -> Dict[str, Any]:
    paths = ("/api/monitoring", "/api/monitpring")
    last_error: Optional[Exception] = None

    for path in paths:
        url = f"http://{host}:{port}{path}"
        try:
            req = urllib.request.Request(url, headers={"Accept": "application/json"})
            with urllib.request.urlopen(req, timeout=timeout) as response:
                data = response.read().decode("utf-8", errors="replace")
            return json.loads(data)
        except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError, json.JSONDecodeError) as exc:
            last_error = exc
            continue

    raise RuntimeError(f"API request failed for {host}:{port} ({last_error})")


def pct_used(free_val: float, total_val: float) -> float:
    if total_val <= 0:
        return 0.0
    used = 100.0 * (1.0 - (free_val / total_val))
    if used < 0:
        return 0.0
    if used > 100:
        return 100.0
    return used


def status_by_upper_threshold(value: float, warn: float, crit: float) -> int:
    if value >= crit:
        return 2
    if value >= warn:
        return 1
    return 0


def status_by_lower_threshold(value: float, warn: float, crit: float) -> int:
    if value <= crit:
        return 2
    if value <= warn:
        return 1
    return 0


def emit(service: str, status: int, perf: str, text: str) -> None:
    print(f'{status} "{service}" {perf} {text}')


def monitoring_to_checkmk(data: Dict[str, Any], host: str) -> int:
    overall_status = 0

    uptime = data.get("uptime", {})
    cpu = data.get("cpu", {})
    memory = data.get("memory", {})
    spiffs = data.get("spiffs", {})
    nvs = data.get("nvs", {})

    uptime_ms = float(uptime.get("ms", 0) or 0)
    uptime_text = str(uptime.get("text", "unknown"))
    uptime_s = int(uptime_ms / 1000.0)
    emit(
        service="Pylontech Uptime",
        status=0,
        perf=f"uptime_s={uptime_s};;;0;",
        text=f"uptime={uptime_text}",
    )

    cpu_valid = bool(cpu.get("valid", False))
    cpu_total = float(cpu.get("load_total", -1) or -1)
    cpu_core0 = float(cpu.get("load_core0", -1) or -1)
    cpu_core1 = float(cpu.get("load_core1", -1) or -1)

    if cpu_valid and cpu_total >= 0:
        cpu_status = status_by_upper_threshold(cpu_total, warn=80.0, crit=90.0)
        overall_status = max(overall_status, cpu_status)
        emit(
            service="Pylontech CPU",
            status=cpu_status,
            perf=(
                f"load_total={cpu_total:.2f};80;90;0;100 "
                f"load_core0={cpu_core0:.2f};85;95;0;100 "
                f"load_core1={cpu_core1:.2f};85;95;0;100"
            ),
            text="CPU load",
        )
    else:
        emit(
            service="Pylontech CPU",
            status=1,
            perf="-",
            text="CPU load not valid yet (first sample window)",
        )
        overall_status = max(overall_status, 1)

    heap_free = float(memory.get("heap_free", 0) or 0)
    heap_total = float(memory.get("heap_total", 0) or 0)
    heap_min = float(memory.get("heap_min", 0) or 0)
    heap_largest = float(memory.get("heap_largest_block", 0) or 0)
    heap_used_percent = pct_used(heap_free, heap_total)
    mem_status = status_by_upper_threshold(heap_used_percent, warn=85.0, crit=95.0)
    # Largest contiguous free block is the real fragmentation/stability predictor:
    # once it drops too low, large allocations fail and the device reboots even
    # though total free heap still looks acceptable.
    frag_warn, frag_crit = 8192.0, 6144.0
    if heap_largest > 0:
        mem_status = max(
            mem_status,
            status_by_lower_threshold(heap_largest, warn=frag_warn, crit=frag_crit),
        )
    overall_status = max(overall_status, mem_status)
    emit(
        service="Pylontech Memory",
        status=mem_status,
        perf=(
            f"heap_used_pct={heap_used_percent:.2f};85;95;0;100 "
            f"heap_free_kb={heap_free/1024.0:.1f};65536;32768;0; "
            f"heap_min={int(heap_min)};;;0; "
            f"heap_largest_block={int(heap_largest)};{int(frag_warn)};{int(frag_crit)};0;"
        ),
        text=(
            f"heap_free={int(heap_free)} bytes, "
            f"heap_total={int(heap_total)} bytes, "
            f"largest_block={int(heap_largest)} bytes, "
            f"min={int(heap_min)} bytes"
        ),
    )

    psram_free = float(memory.get("psram_free", 0) or 0)
    psram_total = float(memory.get("psram_total", 0) or 0)
    psram_used_percent = pct_used(psram_free, psram_total)
    psram_status = status_by_upper_threshold(psram_used_percent, warn=85.0, crit=95.0)
    overall_status = max(overall_status, psram_status)
    emit(
        service="Pylontech PSRAM",
        status=psram_status,
        perf=f"psram_used_pct={psram_used_percent:.2f};85;95;0;100",
        text=(
            f"psram_free={int(psram_free)} bytes, "
            f"psram_total={int(psram_total)} bytes"
        ),
    )

    spiffs_total = float(spiffs.get("total", 0) or 0)
    spiffs_free = float(spiffs.get("free", 0) or 0)
    spiffs_used_percent = pct_used(spiffs_free, spiffs_total)
    spiffs_status = status_by_upper_threshold(spiffs_used_percent, warn=85.0, crit=95.0)
    overall_status = max(overall_status, spiffs_status)
    emit(
        service="Pylontech SPIFFS",
        status=spiffs_status,
        perf=f"spiffs_used_pct={spiffs_used_percent:.2f};85;95;0;100",
        text=(
            f"spiffs_free={int(spiffs_free)} bytes, "
            f"spiffs_total={int(spiffs_total)} bytes"
        ),
    )

    nvs_ok = bool(nvs.get("ok", False))
    if not nvs_ok:
        emit(
            service="Pylontech NVS",
            status=2,
            perf="-",
            text="nvs stats not available",
        )
        overall_status = max(overall_status, 2)
    else:
        nvs_total = float(nvs.get("total", 0) or 0)
        nvs_free = float(nvs.get("free", 0) or 0)
        nvs_used_percent = pct_used(nvs_free, nvs_total)
        nvs_status = status_by_upper_threshold(nvs_used_percent, warn=85.0, crit=95.0)
        overall_status = max(overall_status, nvs_status)
        emit(
            service="Pylontech NVS",
            status=nvs_status,
            perf=f"nvs_used_pct={nvs_used_percent:.2f};85;95;0;100",
            text=(
                f"nvs_free={int(nvs_free)} entries, "
                f"nvs_total={int(nvs_total)} entries"
            ),
        )

    emit(
        service="Pylontech Monitoring API",
        status=overall_status,
        perf="-",
        text=f"host={host}",
    )
    return overall_status


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Checkmk local check for Pylontech /api/monitoring")
    parser.add_argument("--host", required=True, help="ESP host or IP")
    parser.add_argument("--port", type=int, default=80, help="HTTP port (default: 80)")
    parser.add_argument("--timeout", type=float, default=3.0, help="HTTP timeout seconds (default: 3)")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        data = fetch_json(args.host, args.port, args.timeout)
    except Exception as exc:
        emit(
            service="Pylontech Monitoring API",
            status=2,
            perf="-",
            text=f"request failed: {exc}",
        )
        return 2

    return monitoring_to_checkmk(data, args.host)


if __name__ == "__main__":
    sys.exit(main())
