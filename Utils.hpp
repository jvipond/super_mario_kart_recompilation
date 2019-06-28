#include <cstdint>
#include <tuple>

std::tuple<uint32_t, uint32_t> getBankAndOffset( uint32_t addr );

extern "C"
{
	uint32_t convertRuntimeAddressToOffset( uint32_t addr );
}