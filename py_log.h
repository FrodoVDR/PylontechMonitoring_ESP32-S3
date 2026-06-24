#pragma once
#include <Arduino.h>
//#include "web/wp_ui.h"

enum LogLevel {
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_DEBUG
};

void WebLogInit();
void Log(LogLevel lvl, const String& msg);

void WebLog(const String& msg);
String WebLogGet();
void WebLogClear();

bool WebLogFileEnabled();
void WebLogFileEnable(bool enable);
String WebLogFilePath();
bool WebLogFileClear();

//void PersistentLog(const String& msg);
//String PersistentLogDump();
//extern bool persistentLoggingEnabled;

