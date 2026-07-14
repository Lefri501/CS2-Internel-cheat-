#pragma once

namespace Notify {

enum class Type {
	Info,
	Success,
	Warn,
	Error
};

// Push a glass toast (bottom-right). durationSec <= 0 uses default.
void Push(const char* title, const char* message = nullptr, Type type = Type::Info, float durationSec = 0.f);

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

} // namespace Notify
