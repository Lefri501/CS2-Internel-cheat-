#pragma once

#include <cstdint>
#include "../../../external/safetyhook/safetyhook.hpp"

#define CS_CONCATENATE_DETAIL(x, y) x##y
#define CS_CONCATENATE(x, y) CS_CONCATENATE_DETAIL(x, y)

#define CS_CLASS_NO_CONSTRUCTOR(CLASS)	\
CLASS() = delete;						\
CLASS(CLASS&&) = delete;				\
CLASS(const CLASS&) = delete;


#define CS_CLASS_NO_ASSIGNMENT(CLASS)	\
CLASS& operator=(CLASS&&) = delete;		\
CLASS& operator=(const CLASS&) = delete;

#define CS_CLASS_NO_INITIALIZER(CLASS)	\
CS_CLASS_NO_CONSTRUCTOR(CLASS)			\
CS_CLASS_NO_ASSIGNMENT(CLASS)

#define MEM_PAD(SIZE) \
private: \
    char CS_CONCATENATE(pad_, __COUNTER__)[SIZE]; \
public:

// SafetyHook-backed inline hook (replaces MinHook)
template <typename T>
class CInlineHookObj
{
public:
	bool Add(void* pFunction, void* pDetour)
	{
		if (pFunction == nullptr || pDetour == nullptr)
			return false;

		pBaseFn = pFunction;
		pReplaceFn = pDetour;

		m_hook = safetyhook::create_inline(pFunction, pDetour);
		if (!m_hook)
			return false;

		bIsHooked = m_hook.enabled();
		return true;
	}

	bool Replace()
	{
		if (!m_hook || bIsHooked)
			return false;
		if (auto r = m_hook.enable(); !r)
			return false;
		bIsHooked = true;
		return true;
	}

	bool Remove()
	{
		m_hook.reset();
		bIsHooked = false;
		pBaseFn = nullptr;
		pReplaceFn = nullptr;
		return true;
	}

	bool Restore()
	{
		if (!bIsHooked || !m_hook)
			return false;
		if (auto r = m_hook.disable(); !r)
			return false;
		bIsHooked = false;
		return true;
	}

	inline T GetOriginal()
	{
		if (!m_hook)
			return nullptr;
		return m_hook.template original<T>();
	}

	inline bool IsHooked() const
	{
		return bIsHooked && static_cast<bool>(m_hook);
	}

private:
	safetyhook::InlineHook m_hook{};
	bool bIsHooked = false;
	void* pBaseFn = nullptr;
	void* pReplaceFn = nullptr;
};
