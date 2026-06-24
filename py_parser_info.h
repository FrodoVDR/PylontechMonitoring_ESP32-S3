#pragma once
#include <Arduino.h>
#include <vector>
#include "config.h"

// Global INFO storage (for Web UI)
extern InfoData lastParsedInfo;
extern String infoLastStatusMessage;
extern bool infoLastParseOk;
extern unsigned long infoLastStatusMillis;

// INFO parser function
ParseResult parseInfoFrame(int moduleIndex,
                           const String& raw,
                           InfoData& out);
