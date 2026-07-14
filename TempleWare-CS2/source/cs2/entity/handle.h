#pragma once
#include <cstdint>

// @source: https://developer.valvesoftware.com/wiki/Entity_limit#Source_2_limits

#define INVALID_EHANDLE_INDEX 0xFFFFFFFF
#define ENT_ENTRY_MASK 0x7FFF
#define NUM_SERIAL_NUM_SHIFT_BITS 15
#define ENT_MAX_NETWORKED_ENTRY 16384
class CBaseEntity;
class CBaseHandle
{
public:
	CBaseHandle() noexcept :
		nIndex(INVALID_EHANDLE_INDEX) { }

	CBaseHandle(const int nEntry, const int nSerial) noexcept
	{
		nIndex = nEntry | (nSerial << NUM_SERIAL_NUM_SHIFT_BITS);
	}

	bool operator!=(const CBaseHandle& other) const noexcept
	{
		return nIndex != other.nIndex;
	}

	bool operator==(const CBaseHandle& other) const noexcept
	{
		return nIndex == other.nIndex;
	}

	bool operator<(const CBaseHandle& other) const noexcept
	{
		return nIndex < other.nIndex;
	}

	[[nodiscard]] bool valid() const noexcept
	{
		return nIndex != INVALID_EHANDLE_INDEX;
	}

	[[nodiscard]] int index() const noexcept
	{
		return static_cast<int>(nIndex & ENT_ENTRY_MASK);
	}

	[[nodiscard]] int serial_number() const noexcept
	{
		return static_cast<int>(nIndex >> NUM_SERIAL_NUM_SHIFT_BITS);
	}

private:
	std::uint32_t nIndex;

};

class c_handle
{
public:
	c_handle();
	c_handle(const c_handle& other);
	c_handle(unsigned long value);
	c_handle(int iEntry, int iSerialNumber);

	inline int get_index()
	{
		return handler & 0x7FFF;
	}

	inline unsigned long get_handle()
	{
		return handler;
	}

	inline bool is_valid()
	{
		if (handler <= 0 || handler == 0xffffffff)
			return false;

		return true;
	}

protected:
	std::uint32_t handler;
};

inline c_handle::c_handle()
{
	handler = -1;
}

inline c_handle::c_handle(const c_handle& other)
{
	handler = other.handler;
}

inline c_handle::c_handle(unsigned long value)
{
	handler = value;
}