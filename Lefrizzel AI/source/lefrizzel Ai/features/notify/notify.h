#pragma once

namespace Notify {

enum class Type {
	Info,
	Success,
	Warn,
	Error
};

// Bottom-right toast stack. durationSec <= 0 uses default by type.
void Push(const char* title, const char* message = nullptr, Type type = Type::Info, float durationSec = 0.f);

// printf-style body (title fixed).
void PushFmt(Type type, const char* title, const char* fmt, ...);

inline void Info(const char* title, const char* message = nullptr) {
	Push(title, message, Type::Info);
}
inline void Success(const char* title, const char* message = nullptr) {
	Push(title, message, Type::Success);
}
inline void Warn(const char* title, const char* message = nullptr) {
	Push(title, message, Type::Warn);
}
inline void Error(const char* title, const char* message = nullptr) {
	Push(title, message, Type::Error);
}

void Render();
bool HasPending();
void Clear();

// HudChatPrintf — in-game chat line (no-op if pattern miss).
void ChatPrintf(const char* fmt, ...);

// tier0 ConMsg/Msg → developer console (~). Forces LOG_CONSOLE verbosity if gated.
void ConsolePrintf(const char* fmt, ...);

// Same as ConsolePrintf but ConColorMsg first (RGBA 0–255). Falls back to plain.
// Use for feature logs so they stand out from engine spam.
void ConsoleColorPrintf(int r, int g, int b, int a, const char* fmt, ...);

} // namespace Notify
