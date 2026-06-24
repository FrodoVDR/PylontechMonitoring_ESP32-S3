#!/usr/bin/env python3
"""
CheckMK Custom Agent Plugin for PylontechMonitoring ESP32-S3
Runs on CheckMK server, monitors ESP via API over network

Usage:
  Deploy to: ~/local/share/check_mk/agents/special/agent_pylontech
  Then configure via WATO: Datasources > Pylontech Monitoring
"""

import sys
import argparse
import json
import socket
import time
from typing import Optional, Dict, Any
from urllib.error import URLError, HTTPError
from urllib.request import urlopen

__version__ = "2.1.0"


def fetch_monitoring_json(
    host: str, port: int = 80, timeout: int = 3, path: str = "/api/monitoring"
) -> Optional[Dict[str, Any]]:
    """Fetch monitoring data from ESP32 API endpoint."""
    url = f"http://{host}:{port}{path}"
    
    try:
        with urlopen(url, timeout=timeout) as response:
            if response.status == 200:
                return json.loads(response.read().decode("utf-8"))
    except (URLError, HTTPError, socket.timeout, json.JSONDecodeError) as e:
        return None
    except Exception:
        return None
    
    return None


def pct_used(free: int, total: int) -> int:
    """Calculate percentage used from free/total bytes."""
    if total == 0:
        return 0
    return 100 - int((free / total) * 100)


def first_int(mapping: Dict[str, Any], *keys: str, default: int = 0) -> int:
    """Return first existing numeric value from keys as int."""
    for key in keys:
        value = mapping.get(key)
        if isinstance(value, (int, float)):
            return int(value)
    return default


def status_by_upper_threshold(
    value: float, warn: float, crit: float
) -> int:
    """Return Checkmk status: 0=OK, 1=WARN, 2=CRIT."""
    if value >= crit:
        return 2
    elif value >= warn:
        return 1
    return 0


def format_uptime(milliseconds: int) -> str:
    """Convert milliseconds to human-readable uptime string."""
    seconds = milliseconds // 1000
    days = seconds // 86400
    hours = (seconds % 86400) // 3600
    minutes = (seconds % 3600) // 60
    secs = seconds % 60
    
    if days > 0:
        return f"{days}d {hours}h {minutes}m {secs}s"
    elif hours > 0:
        return f"{hours}h {minutes}m {secs}s"
    elif minutes > 0:
        return f"{minutes}m {secs}s"
    else:
        return f"{secs}s"


def monitoring_to_checkmk(
    data: Dict[str, Any],
    host: str,
    heap_free_warn: int = 22528,
    heap_free_crit: int = 16384,
    heap_largest_warn: int = 8192,
    heap_largest_crit: int = 6144,
) -> None:
    """Convert API monitoring data to CheckMK local check format."""
    
    # Default thresholds
    cpu_warn, cpu_crit = 85, 95
    mem_warn, mem_crit = 95, 98
    psram_warn, psram_crit = 85, 95
    spiffs_warn, spiffs_crit = 85, 95
    nvs_warn, nvs_crit = 90, 97
    
    # ===== UPTIME =====
    uptime_ms = data.get("uptime", {}).get("ms", 0)
    uptime_text = format_uptime(uptime_ms)
    print(f'0 "Pylontech Uptime" uptime_ms={uptime_ms} Uptime: {uptime_text}')
    
    # ===== CPU =====
    cpu_data = data.get("cpu", {})
    if cpu_data.get("valid", False):
        cpu_load = cpu_data.get("load_total", 0)
        cpu_core0 = cpu_data.get("load_core0", 0)
        cpu_core1 = cpu_data.get("load_core1", 0)
        cpu_status = status_by_upper_threshold(cpu_load, cpu_warn, cpu_crit)
        perf = f"cpu_load={cpu_load};{cpu_warn};{cpu_crit}|cpu_core0={cpu_core0}|cpu_core1={cpu_core1}"
        status_text = ["OK", "WARN", "CRIT"][cpu_status]
        print(f'{cpu_status} "Pylontech CPU" {perf} {status_text} - CPU load {cpu_load}% (core0 {cpu_core0}%, core1 {cpu_core1}%)')
    else:
        print('1 "Pylontech CPU" - CPU load not yet available (warming)')
    
    # ===== MEMORY (Heap) =====
    # Embedded systems benefit from absolute free-heap thresholds in addition to
    # relative utilization percentages. The largest contiguous free block is the
    # real fragmentation/stability predictor: once it drops too low, large
    # allocations fail and the device reboots even though total free heap still
    # looks acceptable.
    mem_data = data.get("memory", {})
    heap_free = first_int(mem_data, "heap_free", "heap_free_kb")
    heap_total = first_int(mem_data, "heap_total", "heap_total_kb")
    heap_min = first_int(mem_data, "heap_min")
    heap_largest = first_int(mem_data, "heap_largest_block")
    if heap_total > 0:
        heap_used_pct = pct_used(heap_free, heap_total)
        heap_status_pct = status_by_upper_threshold(heap_used_pct, mem_warn, mem_crit)
        if heap_free <= heap_free_crit:
            heap_status_abs = 2
        elif heap_free <= heap_free_warn:
            heap_status_abs = 1
        else:
            heap_status_abs = 0
        if heap_largest > 0 and heap_largest <= heap_largest_crit:
            heap_status_frag = 2
        elif heap_largest > 0 and heap_largest <= heap_largest_warn:
            heap_status_frag = 1
        else:
            heap_status_frag = 0
        heap_status = max(heap_status_pct, heap_status_abs, heap_status_frag)
        perf = (
            f"heap_used_pct={heap_used_pct};{mem_warn};{mem_crit}|"
            f"heap_free={heap_free};{heap_free_warn};{heap_free_crit}|"
            f"heap_total={heap_total}|"
            f"heap_min={heap_min}|"
            f"heap_largest_block={heap_largest};{heap_largest_warn};{heap_largest_crit}"
        )
        status_text = ["OK", "WARN", "CRIT"][heap_status]
        print(
            f'{heap_status} "Pylontech Memory" {perf} '
            f'{status_text} - Heap {heap_used_pct}% used '
            f'({heap_free} free of {heap_total}, '
            f'largest block {heap_largest}, min {heap_min})'
        )
    
    # ===== PSRAM =====
    psram_free = first_int(mem_data, "psram_free", "psram_free_kb")
    psram_total = first_int(mem_data, "psram_total", "psram_total_kb")
    if psram_total > 0:
        psram_used_pct = pct_used(psram_free, psram_total)
        psram_status = status_by_upper_threshold(psram_used_pct, psram_warn, psram_crit)
        perf = f"psram_used_pct={psram_used_pct};{psram_warn};{psram_crit}|psram_free={psram_free}|psram_total={psram_total}"
        status_text = ["OK", "WARN", "CRIT"][psram_status]
        print(f'{psram_status} "Pylontech PSRAM" {perf} {status_text} - PSRAM {psram_used_pct}% used ({psram_free} free of {psram_total})')
    
    # ===== SPIFFS =====
    spiffs_data = data.get("spiffs", {})
    spiffs_total = first_int(spiffs_data, "total", "total_bytes")
    spiffs_used = first_int(spiffs_data, "used", "used_bytes")
    spiffs_free = first_int(spiffs_data, "free", default=max(spiffs_total - spiffs_used, 0))
    if spiffs_total > 0:
        spiffs_used_pct = pct_used(spiffs_free, spiffs_total)
        spiffs_status = status_by_upper_threshold(spiffs_used_pct, spiffs_warn, spiffs_crit)
        perf = f"spiffs_used_pct={spiffs_used_pct};{spiffs_warn};{spiffs_crit}|spiffs_free={spiffs_free}|spiffs_used={spiffs_used}|spiffs_total={spiffs_total}"
        status_text = ["OK", "WARN", "CRIT"][spiffs_status]
        print(f'{spiffs_status} "Pylontech SPIFFS" {perf} {status_text} - {spiffs_used_pct}% used ({spiffs_free}B free of {spiffs_total}B)')
    
    # ===== NVS =====
    nvs_data = data.get("nvs", {})
    if nvs_data.get("ok", False):
        nvs_total = first_int(nvs_data, "total")
        nvs_used = first_int(nvs_data, "used", "entries")
        nvs_free = first_int(nvs_data, "free", default=max(nvs_total - nvs_used, 0))
        nvs_used_pct = pct_used(nvs_free, nvs_total) if nvs_total > 0 else 0
        nvs_status = status_by_upper_threshold(nvs_used_pct, nvs_warn, nvs_crit) if nvs_total > 0 else 0
        perf = f"nvs_used_pct={nvs_used_pct};{nvs_warn};{nvs_crit}|nvs_used={nvs_used}|nvs_free={nvs_free}|nvs_total={nvs_total}"
        status_text = ["OK", "WARN", "CRIT"][nvs_status]
        print(f'{nvs_status} "Pylontech NVS" {perf} {status_text} - NVS {nvs_used_pct}% used ({nvs_used} used of {nvs_total})')
    else:
        print('1 "Pylontech NVS" - NVS not available or corrupted')


def emit_crit_error(error_msg: str) -> None:
    """Emit CRIT status for API error."""
    print(f'2 "Pylontech Monitoring" - API Error: {error_msg}')


def main():
    parser = argparse.ArgumentParser(
        description="CheckMK Custom Agent for PylontechMonitoring ESP32-S3",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--host",
        required=True,
        help="ESP32 hostname or IP address",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=80,
        help="ESP32 HTTP port (default: 80)",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=3,
        help="API request timeout in seconds (default: 3)",
    )
    parser.add_argument(
        "--heap-free-warn",
        type=int,
        default=22528,
        help="WARN when absolute free heap (bytes) drops to/below this (default: 22528)",
    )
    parser.add_argument(
        "--heap-free-crit",
        type=int,
        default=16384,
        help="CRIT when absolute free heap (bytes) drops to/below this (default: 16384)",
    )
    parser.add_argument(
        "--heap-largest-warn",
        type=int,
        default=8192,
        help="WARN when largest contiguous free block (bytes) drops to/below this (default: 8192)",
    )
    parser.add_argument(
        "--heap-largest-crit",
        type=int,
        default=6144,
        help="CRIT when largest contiguous free block (bytes) drops to/below this (default: 6144)",
    )
    parser.add_argument(
        "--version",
        action="version",
        version=f"%(prog)s {__version__}",
    )
    
    args = parser.parse_args()
    
    # Fetch monitoring data
    data = fetch_monitoring_json(args.host, args.port, args.timeout)
    
    if data is None:
        print("<<<local>>>")
        emit_crit_error(
            f"Unable to fetch /api/monitoring from {args.host}:{args.port}"
        )
        sys.exit(1)
    
    # Convert to CheckMK format
    print("<<<local>>>")
    monitoring_to_checkmk(
        data,
        args.host,
        heap_free_warn=args.heap_free_warn,
        heap_free_crit=args.heap_free_crit,
        heap_largest_warn=args.heap_largest_warn,
        heap_largest_crit=args.heap_largest_crit,
    )


if __name__ == "__main__":
    main()
