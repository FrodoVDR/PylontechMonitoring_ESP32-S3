#!/usr/bin/env python3
"""
CheckMK WATO Ruleset Plugin for PylontechMonitoring Custom Agent
Allows configuration of ESP32 monitoring via CheckMK UI

Deploy to:
  ~/local/lib/check_mk/gui/plugins/wato/datasource_programs/agent_pylontech.py
"""

from cmk.gui.i18n import _
from cmk.gui.plugins.wato import (
    HostRulespec,
    rulespec_registry,
)
from cmk.gui.cee.plugins.wato.agent_bakery.type_defs import BakeryHostSettings
from cmk.gui.valuespec import (
    TextInput,
    Integer,
    DropdownChoice,
    Tuple,
    Dictionary,
    HostAddress,
)


def _valuespec_agent_pylontech() -> Dictionary:
    """Define WATO value specification for PylontechMonitoring agent."""
    return Dictionary(
        title=_("Pylontech Monitoring (ESP32-S3)"),
        help=_(
            "Monitor Pylontech battery systems via ESP32-S3 running PylontechMonitoring firmware. "
            "The ESP32 device exposes a JSON API at /api/monitoring with system metrics "
            "(uptime, CPU load per core, heap/PSRAM, SPIFFS, NVS). "
            "This plugin fetches those metrics and converts them to CheckMK services."
        ),
        elements=[
            (
                "host",
                HostAddress(
                    title=_("ESP32 Hostname/IP"),
                    help=_(
                        "Hostname or IP address of the PylontechMonitoring ESP32-S3 device. "
                        "Example: 192.168.88.22 or esp32-pylontech.local"
                    ),
                    allow_empty=False,
                ),
            ),
            (
                "port",
                Integer(
                    title=_("HTTP Port"),
                    help=_("HTTP port on which the ESP32 web server listens (default: 80)"),
                    default_value=80,
                    min_value=1,
                    max_value=65535,
                ),
            ),
            (
                "timeout",
                Integer(
                    title=_("Request Timeout"),
                    help=_("API request timeout in seconds (default: 3)"),
                    default_value=3,
                    min_value=1,
                    max_value=30,
                ),
            ),
            (
                "heap_free_levels",
                Tuple(
                    title=_("Free heap lower levels (bytes)"),
                    help=_(
                        "WARN/CRIT when the absolute free internal heap (heap_free) drops "
                        "to or below these values. Maps to the agent options "
                        "--heap-free-warn / --heap-free-crit."
                    ),
                    elements=[
                        Integer(
                            title=_("Warning at or below"),
                            default_value=22528,
                            unit=_("bytes"),
                            min_value=0,
                        ),
                        Integer(
                            title=_("Critical at or below"),
                            default_value=16384,
                            unit=_("bytes"),
                            min_value=0,
                        ),
                    ],
                ),
            ),
            (
                "heap_largest_levels",
                Tuple(
                    title=_("Largest free block lower levels (bytes)"),
                    help=_(
                        "WARN/CRIT when the largest contiguous free heap block "
                        "(heap_largest_block) drops to or below these values. This is the "
                        "key fragmentation/stability indicator: once it is too small, large "
                        "allocations fail and the device reboots even though total free heap "
                        "still looks fine. Maps to the agent options "
                        "--heap-largest-warn / --heap-largest-crit."
                    ),
                    elements=[
                        Integer(
                            title=_("Warning at or below"),
                            default_value=8192,
                            unit=_("bytes"),
                            min_value=0,
                        ),
                        Integer(
                            title=_("Critical at or below"),
                            default_value=6144,
                            unit=_("bytes"),
                            min_value=0,
                        ),
                    ],
                ),
            ),
        ],
        optional_keys=["port", "timeout", "heap_free_levels", "heap_largest_levels"],
    )


rulespec_registry.register(
    HostRulespec(
        group="datasource_programs",
        name="agent_pylontech",
        title=_("Pylontech Monitoring (Custom Agent)"),
        help=_(
            "Retrieve system monitoring data from PylontechMonitoring ESP32-S3 devices "
            "via the /api/monitoring JSON endpoint. Services include: Uptime, CPU, Memory, PSRAM, SPIFFS, NVS."
        ),
        valuespec=_valuespec_agent_pylontech,
        match="all",
    )
)
