#pragma once
#include <string>

enum LogType {
	Info,
	Warning,
	Error,
	None
};

class Logger {
public:
	// Using std::string for ease of use, overhead is practically non-existent
	static void Log(const char* text, LogType = LogType::Info);
	static void Watermark();
};