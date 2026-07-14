#pragma once

#ifdef _DEBUG
void initDebug();

// AD (hypothesis) debug log — NDJSON to Documents\TempleWare\ad-debug.log + console
// hyp: A/B/C/...  loc: file:func  msg: short tag  dataJson: object body without outer braces ok as full object
void ADLog(const char* hyp, const char* loc, const char* msg, const char* dataJson);
void ADLogf(const char* hyp, const char* loc, const char* msg, const char* fmt, ...);
#else
inline void initDebug() {}
inline void ADLog(const char*, const char*, const char*, const char*) {}
inline void ADLogf(const char*, const char*, const char*, const char*, ...) {}
#endif
