#pragma once

#define CS_CONCATENATE_DETAIL(x, y) x##y
#define CS_CONCATENATE(x, y) CS_CONCATENATE_DETAIL(x, y)

#define MEM_PAD(SIZE) \
private: \
    char CS_CONCATENATE(pad_, __COUNTER__)[SIZE]; \
public:
