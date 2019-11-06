#include "Utils.hpp"
#include <cassert>

std::tuple<uint32_t, uint32_t> getBankAndOffset( uint32_t addr )
{
	auto bank = ( addr & 0xFF0000 ) >> 16;
	auto bank_offset = ( addr & 0x00FFFF );
	return { bank, bank_offset };
}