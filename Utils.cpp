#include "Utils.hpp"
#include <cassert>

std::tuple<uint32_t, uint32_t> getBankAndOffset( uint32_t addr )
{
	auto bank = ( addr & 0xFF0000 ) >> 16;
	auto bank_offset = ( addr & 0x00FFFF );
	return { bank, bank_offset };
}

uint32_t convertRuntimeAddressToOffset( uint32_t addr )
{
	auto [ bank, bank_offset ] = getBankAndOffset( addr );

	if ( addr < 0x400000 )
	{
		return addr;
	}

	auto adjustedAddress = addr;
	if ( 0x40 <= bank && bank <= 0x7d )
	{
		adjustedAddress = addr - 0x400000;
	}

	else if ( 0x80 <= bank && bank <= 0x9f )
	{
		adjustedAddress = addr - 0x800000;
	}
	else if ( 0xa0 <= bank && bank <= 0xbf )
	{
		adjustedAddress = addr - 0xa00000 + 0x200000;
	}

	else if ( 0xc0 <= bank && bank <= 0xfd )
	{
		adjustedAddress = addr - 0xc00000;
	}

	else if ( 0xfe <= bank && bank <= 0xff )
	{
		adjustedAddress = addr - 0xc00000;
	}
	else
	{
		assert( adjustedAddress != addr );
	}

	return adjustedAddress;
}