#include "ppu.hpp"
#include <cstring>
#include <limits>
#include <utility>
#include <string>
#ifdef __EMSCRIPTEN__
#include <GLES3/gl32.h>
#include <emscripten.h>
#else
#include "GL/gl3w.h"
#endif // __EMSCRIPTEN__
#include <SDL_video.h>
#include "../dma/Dma.hpp"
#include "../hardware.hpp"
#include <iostream>

struct range_t {
	struct iterator {
		iterator( int64_t position, int64_t step = 0 ) : position( position ), step( step ) {}
		auto operator*() const -> int64_t { return position; }
		auto operator!=( const iterator& source ) const -> bool { return step > 0 ? position < source.position : position > source.position; }
		auto operator++() -> iterator& { position += step; return *this; }

	private:
		int64_t position;
		const int64_t step;
	};

	struct reverse_iterator {
		reverse_iterator( int64_t position, int64_t step = 0 ) : position( position ), step( step ) {}
		auto operator*() const -> int64_t { return position; }
		auto operator!=( const reverse_iterator& source ) const -> bool { return step > 0 ? position > source.position : position < source.position; }
		auto operator++() -> reverse_iterator& { position -= step; return *this; }

	private:
		int64_t position;
		const int64_t step;
	};

	auto begin() const -> iterator { return { origin, stride }; }
	auto end() const -> iterator { return { target }; }

	auto rbegin() const -> reverse_iterator { return { target - stride, stride }; }
	auto rend() const -> reverse_iterator { return { origin - stride }; }

	int64_t origin;
	int64_t target;
	int64_t stride;
};

inline auto range( int64_t size ) {
	return range_t{ 0, size, 1 };
}

inline auto range( int64_t offset, int64_t size ) {
	return range_t{ offset, size, 1 };
}

inline auto range( int64_t offset, int64_t size, int64_t step ) {
	return range_t{ offset, size, step };
}

namespace memory
{
	template<typename T = uint8_t> inline auto fill( void* target, uint capacity, const T& value = {} )->T*;
	template<typename T> inline auto assign( T* target ) -> void {}
	template<typename T, typename U, typename... P> inline auto assign( T* target, const U& value, P&&... p ) -> void;

	template<typename T> auto fill( void* target, uint capacity, const T& value ) -> T* {
		auto t = (T*)target;
		while ( capacity-- ) *t++ = value;
		return (T*)target;
	}

	template<typename T, typename U, typename... P> auto assign( T* target, const U& value, P&&... p ) -> void {
		*target++ = value;
		assign( target, std::forward<P>( p )... );
	}

}

#define PPU PPUfast
#define ppu ppufast

PPU ppu;

auto PPU::latchCounters( uint hcounter, uint vcounter ) -> void {
	io.hcounter = hcounter;
	io.vcounter = vcounter;
	latch.counters = 1;
}

auto PPU::latchCounters() -> void {
	//io.hcounter = cpu.hdot();
	//io.vcounter = cpu.vcounter();
	latch.counters = 1;
}

auto PPU::vramAddress() const -> uint {
	uint address = io.vramAddress;
	switch ( io.vramMapping ) {
	case 0: return address & 0x7fff;
	case 1: return address & 0x7f00 | address << 3 & 0x00f8 | address >> 5 & 7;
	case 2: return address & 0x7e00 | address << 3 & 0x01f8 | address >> 6 & 7;
	case 3: return address & 0x7c00 | address << 3 & 0x03f8 | address >> 7 & 7;
	}
	return 0;
	//unreachable;
}

auto PPU::readVRAM() -> uint16 {
	if ( !io.displayDisable /*&& cpu.vcounter() < vdisp()*/ ) return 0x0000;
	auto address = vramAddress();
	return vram[ address ];
}

template<bool Byte>
auto PPU::writeVRAM( uint8 data ) -> void {
	if ( !io.displayDisable /*&& cpu.vcounter() < vdisp()*/ && !noVRAMBlocking() ) return;
	//Line::flush();
	auto address = vramAddress();
	if constexpr ( Byte == 0 ) {
		vram[ address ] = vram[ address ] & 0xff00 | data << 0;
	}
	if constexpr ( Byte == 1 ) {
		vram[ address ] = vram[ address ] & 0x00ff | data << 8;
	}
}

auto PPU::readOAM( uint10 address ) -> uint8 {
	if ( !io.displayDisable /*&& cpu.vcounter() < vdisp()*/ ) address = latch.oamAddress;
	return readObject( address );
}

auto PPU::writeOAM( uint10 address, uint8 data ) -> void {
	//Line::flush();
	//0x0218: Uniracers (2-player mode) hack; requires cycle timing for latch.oamAddress to be correct
	//if ( !io.displayDisable /*&& cpu.vcounter() < vdisp()*/ ) address = 0x0218;  //latch.oamAddress;
	return writeObject( address, data );
}

template<bool Byte>
auto PPU::readCGRAM( uint8 address ) -> uint8 {
	if ( !io.displayDisable
		/*&& cpu.vcounter() > 0 && cpu.vcounter() < vdisp()
		&& cpu.hcounter() >= 88 && cpu.hcounter() < 1096*/
		) address = latch.cgramAddress;
	if constexpr ( Byte == 0 ) {
		return cgram[ address ] >> 0;
	}
	if constexpr ( Byte == 1 ) {
		return cgram[ address ] >> 8;
	}
}

auto PPU::writeCGRAM( uint8 address, uint15 data ) -> void {
	// TODO - check this and make sure that never latching is ok
	//if ( !io.displayDisable
	//	/*&& cpu.vcounter() > 0 && cpu.vcounter() < vdisp()
	//	&& cpu.hcounter() >= 88 && cpu.hcounter() < 1096*/
	//	) address = latch.cgramAddress;
	cgram[ address ] = data;
}

auto PPU::readIO( uint address, uint8 data ) -> uint8 {
	//cpu.synchronizePPU();

	switch ( address & 0xffff ) {

	case 0x2104: case 0x2105: case 0x2106: case 0x2108:
	case 0x2109: case 0x210a: case 0x2114: case 0x2115:
	case 0x2116: case 0x2118: case 0x2119: case 0x211a:
	case 0x2124: case 0x2125: case 0x2126: case 0x2128:
	case 0x2129: case 0x212a: {
		return latch.ppu1.mdr;
	}

	case 0x2134: {  //MPYL
		uint result = (int16)io.mode7.a * (int8)( io.mode7.b >> 8 );
		return latch.ppu1.mdr = result >> 0;
	}

	case 0x2135: {  //MPYM
		uint result = (int16)io.mode7.a * (int8)( io.mode7.b >> 8 );
		return latch.ppu1.mdr = result >> 8;
	}

	case 0x2136: {  //MPYH
		uint result = (int16)io.mode7.a * (int8)( io.mode7.b >> 8 );
		return latch.ppu1.mdr = result >> 16;
	}

	case 0x2137: {  //SLHV
		//if ( cpu.pio() & 0x80 ) latchCounters();
		return data;  //CPU MDR
	}

	case 0x2138: {  //OAMDATAREAD
		data = readOAM( io.oamAddress );
		io.oamAddress = io.oamAddress + 1 & 0x3ff;
		oamSetFirstObject();
		return latch.ppu1.mdr = data;
	}

	case 0x2139: {  //VMDATALREAD
		data = latch.vram >> 0;
		if ( io.vramIncrementMode == 0 ) {
			latch.vram = readVRAM();
			io.vramAddress += io.vramIncrementSize;
		}
		return latch.ppu1.mdr = data;
	}

	case 0x213a: {  //VMDATAHREAD
		data = latch.vram >> 8;
		if ( io.vramIncrementMode == 1 ) {
			latch.vram = readVRAM();
			io.vramAddress += io.vramIncrementSize;
		}
		return latch.ppu1.mdr = data;
	}

	case 0x213b: {  //CGDATAREAD
		if ( io.cgramAddressLatch == 0 ) {
			io.cgramAddressLatch = 1;
			latch.ppu2.mdr = readCGRAM<0>( io.cgramAddress );
		}
		else {
			io.cgramAddressLatch = 0;
			latch.ppu2.mdr = readCGRAM<1>( io.cgramAddress++ ) & 0x7f | latch.ppu2.mdr & 0x80;
		}
		return latch.ppu2.mdr;
	}

	case 0x213c: {  //OPHCT
		if ( latch.hcounter == 0 ) {
			latch.hcounter = 1;
			latch.ppu2.mdr = io.hcounter;
		}
		else {
			latch.hcounter = 0;
			latch.ppu2.mdr = io.hcounter >> 8 | latch.ppu2.mdr & 0xfe;
		}
		return latch.ppu2.mdr;
	}

	case 0x213d: {  //OPVCT
		if ( latch.vcounter == 0 ) {
			latch.vcounter = 1;
			latch.ppu2.mdr = io.vcounter;
		}
		else {
			latch.vcounter = 0;
			latch.ppu2.mdr = io.vcounter >> 8 | latch.ppu2.mdr & 0xfe;
		}
		return latch.ppu2.mdr;
	}

	case 0x213e: {  //STAT77
		latch.ppu1.mdr = 0x01 | io.obj.rangeOver << 6 | io.obj.timeOver << 7;
		return latch.ppu1.mdr;
	}

	case 0x213f: {  //STAT78
		latch.hcounter = 0;
		latch.vcounter = 0;
		latch.ppu2.mdr &= 1 << 5;
		latch.ppu2.mdr |= 0x03 ;
		//latch.ppu2.mdr |= 0x03 | Region::PAL() << 4 | field() << 7;
		/*if ( !( cpu.pio() & 0x80 ) ) {
			latch.ppu2.mdr |= 1 << 6;
		}
		else*/ {
			latch.ppu2.mdr |= latch.counters << 6;
			latch.counters = 0;
		}
		return latch.ppu2.mdr;
	}

	}

	return data;
}

auto PPU::writeIO( uint address, uint8 data ) -> void {
	//cpu.synchronizePPU();

	switch ( address & 0xffff ) {

	case 0x2100: {  //INIDISP
		if ( io.displayDisable /*&& cpu.vcounter() == vdisp()*/ ) oamAddressReset();
		io.displayBrightness = data >> 0 & 15;
		io.displayDisable = data >> 7 & 1;
		return;
	}

	case 0x2101: {  //OBSEL
		io.obj.tiledataAddress = data << 13 & 0x6000;
		io.obj.nameselect = data >> 3 & 3;
		io.obj.baseSize = data >> 5 & 7;
		return;
	}

	case 0x2102: {  //OAMADDL
		io.oamBaseAddress = ( io.oamBaseAddress & 0x0200 ) | data << 1;
		oamAddressReset();
		return;
	}

	case 0x2103: {  //OAMADDH
		io.oamBaseAddress = ( data & 1 ) << 9 | io.oamBaseAddress & 0x01fe;
		io.oamPriority = data >> 7 & 1;
		oamAddressReset();
		return;
	}

	case 0x2104: {  //OAMDATA
		bool latchBit = io.oamAddress & 1;
		uint address = io.oamAddress;
		io.oamAddress = io.oamAddress + 1 & 0x3ff;
		if ( latchBit == 0 ) latch.oam = data;
		if ( address & 0x200 ) {
			writeOAM( address, data );
		}
		else if ( latchBit == 1 ) {
			writeOAM( ( address & ~1 ) + 0, latch.oam );
			writeOAM( ( address & ~1 ) + 1, data );
		}
		oamSetFirstObject();
		return;
	}

	case 0x2105: {  //BGMODE
		io.bgMode = data >> 0 & 7;
		io.bgPriority = data >> 3 & 1;
		io.bg1.tileSize = data >> 4 & 1;
		io.bg2.tileSize = data >> 5 & 1;
		io.bg3.tileSize = data >> 6 & 1;
		io.bg4.tileSize = data >> 7 & 1;
		updateVideoMode();
		return;
	}

	case 0x2106: {  //MOSAIC
		io.bg1.mosaicEnable = data >> 0 & 1;
		io.bg2.mosaicEnable = data >> 1 & 1;
		io.bg3.mosaicEnable = data >> 2 & 1;
		io.bg4.mosaicEnable = data >> 3 & 1;
		io.mosaicSize = data >> 4 & 15;
		return;
	}

	case 0x2107: {  //BG1SC
		io.bg1.screenSize = data >> 0 & 3;
		io.bg1.screenAddress = data << 8 & 0x7c00;
		return;
	}

	case 0x2108: {  //BG2SC
		io.bg2.screenSize = data >> 0 & 3;
		io.bg2.screenAddress = data << 8 & 0x7c00;
		return;
	}

	case 0x2109: {  //BG3SC
		io.bg3.screenSize = data >> 0 & 3;
		io.bg3.screenAddress = data << 8 & 0x7c00;
		return;
	}

	case 0x210a: {  //BG4SC
		io.bg4.screenSize = data >> 0 & 3;
		io.bg4.screenAddress = data << 8 & 0x7c00;
		return;
	}

	case 0x210b: {  //BG12NBA
		io.bg1.tiledataAddress = data << 12 & 0x7000;
		io.bg2.tiledataAddress = data << 8 & 0x7000;
		return;
	}

	case 0x210c: {  //BG34NBA
		io.bg3.tiledataAddress = data << 12 & 0x7000;
		io.bg4.tiledataAddress = data << 8 & 0x7000;
		return;
	}

	case 0x210d: {  //BG1HOFS
		io.mode7.hoffset = data << 8 | latch.mode7;
		latch.mode7 = data;

		io.bg1.hoffset = data << 8 | ( latch.ppu1.bgofs & ~7 ) | ( latch.ppu2.bgofs & 7 );
		latch.ppu1.bgofs = data;
		latch.ppu2.bgofs = data;
		return;
	}

	case 0x210e: {  //BG1VOFS
		io.mode7.voffset = data << 8 | latch.mode7;
		latch.mode7 = data;

		io.bg1.voffset = data << 8 | latch.ppu1.bgofs;
		latch.ppu1.bgofs = data;
		return;
	}

	case 0x210f: {  //BG2HOFS
		io.bg2.hoffset = data << 8 | ( latch.ppu1.bgofs & ~7 ) | ( latch.ppu2.bgofs & 7 );
		latch.ppu1.bgofs = data;
		latch.ppu2.bgofs = data;
		return;
	}

	case 0x2110: {  //BG2VOFS
		io.bg2.voffset = data << 8 | latch.ppu1.bgofs;
		latch.ppu1.bgofs = data;
		return;
	}

	case 0x2111: {  //BG3HOFS
		io.bg3.hoffset = data << 8 | ( latch.ppu1.bgofs & ~7 ) | ( latch.ppu2.bgofs & 7 );
		latch.ppu1.bgofs = data;
		latch.ppu2.bgofs = data;
		return;
	}

	case 0x2112: {  //BG3VOFS
		io.bg3.voffset = data << 8 | latch.ppu1.bgofs;
		latch.ppu1.bgofs = data;
		return;
	}

	case 0x2113: {  //BG4HOFS
		io.bg4.hoffset = data << 8 | ( latch.ppu1.bgofs & ~7 ) | ( latch.ppu2.bgofs & 7 );
		latch.ppu1.bgofs = data;
		latch.ppu2.bgofs = data;
		return;
	}

	case 0x2114: {  //BG4VOFS
		io.bg4.voffset = data << 8 | latch.ppu1.bgofs;
		latch.ppu1.bgofs = data;
		return;
	}

	case 0x2115: {  //VMAIN
		static const uint size[ 4 ] = { 1, 32, 128, 128 };
		io.vramIncrementSize = size[ data & 3 ];
		io.vramMapping = data >> 2 & 3;
		io.vramIncrementMode = data >> 7 & 1;
		return;
	}

	case 0x2116: {  //VMADDL
		io.vramAddress = io.vramAddress & 0xff00 | data << 0;
		latch.vram = readVRAM();
		return;
	}

	case 0x2117: {  //VMADDH
		io.vramAddress = io.vramAddress & 0x00ff | data << 8;
		latch.vram = readVRAM();
		return;
	}

	case 0x2118: {  //VMDATAL
		writeVRAM<0>( data );
		if ( io.vramIncrementMode == 0 ) io.vramAddress += io.vramIncrementSize;
		return;
	}

	case 0x2119: {  //VMDATAH
		writeVRAM<1>( data );
		if ( io.vramIncrementMode == 1 ) io.vramAddress += io.vramIncrementSize;
		return;
	}

	case 0x211a: {  //M7SEL
		io.mode7.hflip = data >> 0 & 1;
		io.mode7.vflip = data >> 1 & 1;
		io.mode7.repeat = data >> 6 & 3;
		return;
	}

	case 0x211b: {  //M7A
		io.mode7.a = data << 8 | latch.mode7;
		latch.mode7 = data;
		return;
	}

	case 0x211c: {  //M7B
		io.mode7.b = data << 8 | latch.mode7;
		latch.mode7 = data;
		return;
	}

	case 0x211d: {  //M7C
		io.mode7.c = data << 8 | latch.mode7;
		latch.mode7 = data;
		return;
	}

	case 0x211e: {  //M7D
		io.mode7.d = data << 8 | latch.mode7;
		latch.mode7 = data;
		return;
	}

	case 0x211f: {  //M7X
		io.mode7.x = data << 8 | latch.mode7;
		latch.mode7 = data;
		return;
	}

	case 0x2120: {  //M7Y
		io.mode7.y = data << 8 | latch.mode7;
		latch.mode7 = data;
		return;
	}

	case 0x2121: {  //CGADD
		io.cgramAddress = data;
		io.cgramAddressLatch = 0;
		return;
	}

	case 0x2122: {  //CGDATA
		if ( io.cgramAddressLatch == 0 ) {
			io.cgramAddressLatch = 1;
			latch.cgram = data;
		}
		else {
			io.cgramAddressLatch = 0;
			writeCGRAM( io.cgramAddress++, ( data & 0x7f ) << 8 | latch.cgram );
		}
		return;
	}

	case 0x2123: {  //W12SEL
		io.bg1.window.oneInvert = data >> 0 & 1;
		io.bg1.window.oneEnable = data >> 1 & 1;
		io.bg1.window.twoInvert = data >> 2 & 1;
		io.bg1.window.twoEnable = data >> 3 & 1;
		io.bg2.window.oneInvert = data >> 4 & 1;
		io.bg2.window.oneEnable = data >> 5 & 1;
		io.bg2.window.twoInvert = data >> 6 & 1;
		io.bg2.window.twoEnable = data >> 7 & 1;
		return;
	}

	case 0x2124: {  //W34SEL
		io.bg3.window.oneInvert = data >> 0 & 1;
		io.bg3.window.oneEnable = data >> 1 & 1;
		io.bg3.window.twoInvert = data >> 2 & 1;
		io.bg3.window.twoEnable = data >> 3 & 1;
		io.bg4.window.oneInvert = data >> 4 & 1;
		io.bg4.window.oneEnable = data >> 5 & 1;
		io.bg4.window.twoInvert = data >> 6 & 1;
		io.bg4.window.twoEnable = data >> 7 & 1;
		return;
	}

	case 0x2125: {  //WOBJSEL
		io.obj.window.oneInvert = data >> 0 & 1;
		io.obj.window.oneEnable = data >> 1 & 1;
		io.obj.window.twoInvert = data >> 2 & 1;
		io.obj.window.twoEnable = data >> 3 & 1;
		io.col.window.oneInvert = data >> 4 & 1;
		io.col.window.oneEnable = data >> 5 & 1;
		io.col.window.twoInvert = data >> 6 & 1;
		io.col.window.twoEnable = data >> 7 & 1;
		return;
	}

	case 0x2126: {  //WH0
		io.window.oneLeft = data;
		return;
	}

	case 0x2127: {  //WH1
		io.window.oneRight = data;
		return;
	}

	case 0x2128: {  //WH2
		io.window.twoLeft = data;
		return;
	}

	case 0x2129: {  //WH3
		io.window.twoRight = data;
		return;
	}

	case 0x212a: {  //WBGLOG
		io.bg1.window.mask = data >> 0 & 3;
		io.bg2.window.mask = data >> 2 & 3;
		io.bg3.window.mask = data >> 4 & 3;
		io.bg4.window.mask = data >> 6 & 3;
		return;
	}

	case 0x212b: {  //WOBJLOG
		io.obj.window.mask = data >> 0 & 3;
		io.col.window.mask = data >> 2 & 3;
		return;
	}

	case 0x212c: {  //TM
		io.bg1.aboveEnable = data >> 0 & 1;
		io.bg2.aboveEnable = data >> 1 & 1;
		io.bg3.aboveEnable = data >> 2 & 1;
		io.bg4.aboveEnable = data >> 3 & 1;
		io.obj.aboveEnable = data >> 4 & 1;
		return;
	}

	case 0x212d: {  //TS
		io.bg1.belowEnable = data >> 0 & 1;
		io.bg2.belowEnable = data >> 1 & 1;
		io.bg3.belowEnable = data >> 2 & 1;
		io.bg4.belowEnable = data >> 3 & 1;
		io.obj.belowEnable = data >> 4 & 1;
		return;
	}

	case 0x212e: {  //TMW
		io.bg1.window.aboveEnable = data >> 0 & 1;
		io.bg2.window.aboveEnable = data >> 1 & 1;
		io.bg3.window.aboveEnable = data >> 2 & 1;
		io.bg4.window.aboveEnable = data >> 3 & 1;
		io.obj.window.aboveEnable = data >> 4 & 1;
		return;
	}

	case 0x212f: {  //TSW
		io.bg1.window.belowEnable = data >> 0 & 1;
		io.bg2.window.belowEnable = data >> 1 & 1;
		io.bg3.window.belowEnable = data >> 2 & 1;
		io.bg4.window.belowEnable = data >> 3 & 1;
		io.obj.window.belowEnable = data >> 4 & 1;
		return;
	}

	case 0x2130: {  //CGWSEL
		io.col.directColor = data >> 0 & 1;
		io.col.blendMode = data >> 1 & 1;
		io.col.window.belowMask = data >> 4 & 3;
		io.col.window.aboveMask = data >> 6 & 3;
		return;
	}

	case 0x2131: {  //CGADDSUB
		io.col.enable[ Source::BG1 ] = data >> 0 & 1;
		io.col.enable[ Source::BG2 ] = data >> 1 & 1;
		io.col.enable[ Source::BG3 ] = data >> 2 & 1;
		io.col.enable[ Source::BG4 ] = data >> 3 & 1;
		io.col.enable[ Source::OBJ1 ] = 0;
		io.col.enable[ Source::OBJ2 ] = data >> 4 & 1;
		io.col.enable[ Source::COL ] = data >> 5 & 1;
		io.col.halve = data >> 6 & 1;
		io.col.mathMode = data >> 7 & 1;
		return;
	}

	case 0x2132: {  //COLDATA
		if ( data & 0x20 ) io.col.fixedColor = io.col.fixedColor & 0b11111'11111'00000 | ( data & 31 ) << 0;
		if ( data & 0x40 ) io.col.fixedColor = io.col.fixedColor & 0b11111'00000'11111 | ( data & 31 ) << 5;
		if ( data & 0x80 ) io.col.fixedColor = io.col.fixedColor & 0b00000'11111'11111 | ( data & 31 ) << 10;
		return;
	}

	case 0x2133: {  //SETINI
		io.interlace = data >> 0 & 1;
		io.obj.interlace = data >> 1 & 1;
		io.overscan = data >> 2 & 1;
		io.pseudoHires = data >> 3 & 1;
		io.extbg = data >> 6 & 1;
		updateVideoMode();
		return;
	}

	}
}

auto PPU::updateVideoMode() -> void {
	/*ppubase.display.vdisp = !io.overscan ? 225 : 240;*/

	switch ( io.bgMode ) {
	case 0:
		io.bg1.tileMode = TileMode::BPP2;
		io.bg2.tileMode = TileMode::BPP2;
		io.bg3.tileMode = TileMode::BPP2;
		io.bg4.tileMode = TileMode::BPP2;
		memory::assign( io.bg1.priority, 8, 11 );
		memory::assign( io.bg2.priority, 7, 10 );
		memory::assign( io.bg3.priority, 2, 5 );
		memory::assign( io.bg4.priority, 1, 4 );
		memory::assign( io.obj.priority, 3, 6, 9, 12 );
		break;

	case 1:
		io.bg1.tileMode = TileMode::BPP4;
		io.bg2.tileMode = TileMode::BPP4;
		io.bg3.tileMode = TileMode::BPP2;
		io.bg4.tileMode = TileMode::Inactive;
		if ( io.bgPriority ) {
			memory::assign( io.bg1.priority, 5, 8 );
			memory::assign( io.bg2.priority, 4, 7 );
			memory::assign( io.bg3.priority, 1, 10 );
			memory::assign( io.obj.priority, 2, 3, 6, 9 );
		}
		else {
			memory::assign( io.bg1.priority, 6, 9 );
			memory::assign( io.bg2.priority, 5, 8 );
			memory::assign( io.bg3.priority, 1, 3 );
			memory::assign( io.obj.priority, 2, 4, 7, 10 );
		}
		break;

	case 2:
		io.bg1.tileMode = TileMode::BPP4;
		io.bg2.tileMode = TileMode::BPP4;
		io.bg3.tileMode = TileMode::Inactive;
		io.bg4.tileMode = TileMode::Inactive;
		memory::assign( io.bg1.priority, 3, 7 );
		memory::assign( io.bg2.priority, 1, 5 );
		memory::assign( io.obj.priority, 2, 4, 6, 8 );
		break;

	case 3:
		io.bg1.tileMode = TileMode::BPP8;
		io.bg2.tileMode = TileMode::BPP4;
		io.bg3.tileMode = TileMode::Inactive;
		io.bg4.tileMode = TileMode::Inactive;
		memory::assign( io.bg1.priority, 3, 7 );
		memory::assign( io.bg2.priority, 1, 5 );
		memory::assign( io.obj.priority, 2, 4, 6, 8 );
		break;

	case 4:
		io.bg1.tileMode = TileMode::BPP8;
		io.bg2.tileMode = TileMode::BPP2;
		io.bg3.tileMode = TileMode::Inactive;
		io.bg4.tileMode = TileMode::Inactive;
		memory::assign( io.bg1.priority, 3, 7 );
		memory::assign( io.bg2.priority, 1, 5 );
		memory::assign( io.obj.priority, 2, 4, 6, 8 );
		break;

	case 5:
		io.bg1.tileMode = TileMode::BPP4;
		io.bg2.tileMode = TileMode::BPP2;
		io.bg3.tileMode = TileMode::Inactive;
		io.bg4.tileMode = TileMode::Inactive;
		memory::assign( io.bg1.priority, 3, 7 );
		memory::assign( io.bg2.priority, 1, 5 );
		memory::assign( io.obj.priority, 2, 4, 6, 8 );
		break;

	case 6:
		io.bg1.tileMode = TileMode::BPP4;
		io.bg2.tileMode = TileMode::Inactive;
		io.bg3.tileMode = TileMode::Inactive;
		io.bg4.tileMode = TileMode::Inactive;
		memory::assign( io.bg1.priority, 2, 5 );
		memory::assign( io.obj.priority, 1, 3, 4, 6 );
		break;

	case 7:
		if ( !io.extbg ) {
			io.bg1.tileMode = TileMode::Mode7;
			io.bg2.tileMode = TileMode::Inactive;
			io.bg3.tileMode = TileMode::Inactive;
			io.bg4.tileMode = TileMode::Inactive;
			memory::assign( io.bg1.priority, 2 );
			memory::assign( io.obj.priority, 1, 3, 4, 5 );
		}
		else {
			io.bg1.tileMode = TileMode::Mode7;
			io.bg2.tileMode = TileMode::Mode7;
			io.bg3.tileMode = TileMode::Inactive;
			io.bg4.tileMode = TileMode::Inactive;
			memory::assign( io.bg1.priority, 3 );
			memory::assign( io.bg2.priority, 1, 5 );
			memory::assign( io.obj.priority, 2, 4, 6, 7 );
		}
		break;
	}
}


uint PPU::Line::start = 0;
uint PPU::Line::count = 0;

auto PPU::Line::flush() -> void {
	if ( Line::count ) {
		if ( ppu.hdScale() > 1 ) cacheMode7HD();
#pragma omp parallel for if(Line::count >= 8)
		for ( uint y = 0; y < Line::count; y++ ) {
			if ( ppu.deinterlace() ) {
				if ( !ppu.interlace() ) {
					//some games enable interlacing in 240p mode, just force these to even fields
					ppu.lines[ Line::start + y ].render( 0 );
				}
				else {
					//for actual interlaced frames, render both fields every farme for 480i -> 480p
					ppu.lines[ Line::start + y ].render( 0 );
					ppu.lines[ Line::start + y ].render( 1 );
				}
			}
			else {
				//standard 240p (progressive) and 480i (interlaced) rendering
				ppu.lines[ Line::start + y ].render( 0/*ppu.field()*/ );
			}
		}
		Line::start = 0;
		Line::count = 0;
	}
}

auto PPU::Line::cache() -> void {
	cacheBackground( ppu.io.bg1 );
	cacheBackground( ppu.io.bg2 );
	cacheBackground( ppu.io.bg3 );
	cacheBackground( ppu.io.bg4 );

	//uint y = 0;//ppu.vcounter();
	//if ( ppu.io.displayDisable || y >= ppu.vdisp() ) {
	//	io.displayDisable = true;
	//}
	//else 
	{
		memcpy( &io, &ppu.io, sizeof( io ) );
		memcpy( &cgram, &ppu.cgram, sizeof( cgram ) );
	}
	if ( !Line::count ) Line::start = y;
	Line::count++;
}

auto PPU::Line::render( bool fieldID ) -> void {
	this->fieldID = fieldID;
	uint y = this->y + ( !ppu.latch.overscan ? 7 : 0 );

	auto hd = ppu.hd();
	auto ss = ppu.ss();
	auto scale = ppu.hdScale();
	auto output = ppu.output + ( !hd
		? ( y * 1024 + ( ppu.interlace() && field() ? 512 : 0 ) )
		: ( y * 256 * scale * scale )
		);
	auto width = ( !hd
		? ( !ppu.hires() ? 256 : 512 )
		: ( 256 * scale * scale ) );

	if ( io.displayDisable ) {
		memory::fill<uint16>( output, width );
		return;
	}

	bool hires = io.pseudoHires || io.bgMode == 5 || io.bgMode == 6;
	uint16 aboveColor = cgram[ 0 ];
	uint16 belowColor = hires ? cgram[ 0 ] : io.col.fixedColor;
	uint xa = ( hd || ss ) && ppu.interlace() && field() ? 256 * scale * scale / 2 : 0;
	uint xb = !( hd || ss ) ? 256 : ppu.interlace() && !field() ? 256 * scale * scale / 2 : 256 * scale * scale;
	for ( uint x = xa; x < xb; x++ ) {
		above[ x ] = { Source::COL, 0, aboveColor };
		below[ x ] = { Source::COL, 0, belowColor };
	}

	//hack: generally, renderBackground/renderObject ordering do not matter.
	//but for HD mode 7, a larger grid of pixels are generated, and so ordering ends up mattering.
	//as a hack for Mohawk & Headphone Jack, we reorder things for BG2 to render properly.
	//longer-term, we need to devise a better solution that can work for every game.
	renderBackground( io.bg1, Source::BG1 );
	if ( io.extbg == 0 ) renderBackground( io.bg2, Source::BG2 );
	renderBackground( io.bg3, Source::BG3 );
	renderBackground( io.bg4, Source::BG4 );
	renderObject( io.obj );
	if ( io.extbg == 1 ) renderBackground( io.bg2, Source::BG2 );
	renderWindow( io.col.window, io.col.window.aboveMask, windowAbove );
	renderWindow( io.col.window, io.col.window.belowMask, windowBelow );

	auto luma = ppu.lightTable[ io.displayBrightness ];
	uint curr = 0, prev = 0;
	if ( hd ) for ( uint x : range( 256 * scale * scale ) ) {
		*output++ = luma[ pixel( x / scale & 255, above[ x ], below[ x ] ) ];
	}
	else if ( width == 256 ) for ( uint x : range( 256 ) ) {
		*output++ = luma[ pixel( x, above[ x ], below[ x ] ) ];
	}
	else if ( !hires ) for ( uint x : range( 256 ) ) {
		auto color = luma[ pixel( x, above[ x ], below[ x ] ) ];
		*output++ = color;
		*output++ = color;
	}
	/*else if ( !configuration.video.blurEmulation ) for ( uint x : range( 256 ) ) {
		*output++ = luma[ pixel( x, below[ x ], above[ x ] ) ];
		*output++ = luma[ pixel( x, above[ x ], below[ x ] ) ];
	}*/
	else for ( uint x : range( 256 ) ) {
		curr = luma[ pixel( x, below[ x ], above[ x ] ) ];
		*output++ = ( prev + curr - ( ( prev ^ curr ) & 0x0421 ) ) >> 1;
		prev = curr;
		curr = luma[ pixel( x, above[ x ], below[ x ] ) ];
		*output++ = ( prev + curr - ( ( prev ^ curr ) & 0x0421 ) ) >> 1;
		prev = curr;
	}
}

auto PPU::Line::pixel( uint x, Pixel above, Pixel below ) const -> uint16 {
	if ( !windowAbove[ x ] ) above.color = 0x0000;
	if ( !windowBelow[ x ] ) return above.color;
	if ( !io.col.enable[ above.source ] ) return above.color;
	if ( !io.col.blendMode ) return blend( above.color, io.col.fixedColor, io.col.halve && windowAbove[ x ] );
	return blend( above.color, below.color, io.col.halve && windowAbove[ x ] && below.source != Source::COL );
}

auto PPU::Line::blend( uint x, uint y, bool halve ) const -> uint16 {
	if ( !io.col.mathMode ) {  //add
		if ( !halve ) {
			uint sum = x + y;
			uint carry = ( sum - ( ( x ^ y ) & 0x0421 ) ) & 0x8420;
			return ( sum - carry ) | ( carry - ( carry >> 5 ) );
		}
		else {
			return ( x + y - ( ( x ^ y ) & 0x0421 ) ) >> 1;
		}
	}
	else {  //sub
		uint diff = x - y + 0x8420;
		uint borrow = ( diff - ( ( x ^ y ) & 0x8420 ) ) & 0x8420;
		if ( !halve ) {
			return   ( diff - borrow ) & ( borrow - ( borrow >> 5 ) );
		}
		else {
			return ( ( ( diff - borrow ) & ( borrow - ( borrow >> 5 ) ) ) & 0x7bde ) >> 1;
		}
	}
}

auto PPU::Line::directColor( uint paletteIndex, uint paletteColor ) const -> uint16 {
	//paletteIndex = bgr
	//paletteColor = BBGGGRRR
	//output       = 0 BBb00 GGGg0 RRRr0
	return ( paletteColor << 2 & 0x001c ) + ( paletteIndex << 1 & 0x0002 )   //R
		+ ( paletteColor << 4 & 0x0380 ) + ( paletteIndex << 5 & 0x0040 )   //G
		+ ( paletteColor << 7 & 0x6000 ) + ( paletteIndex << 10 & 0x1000 );  //B
}

auto PPU::Line::plotAbove( uint x, uint8 source, uint8 priority, uint16 color ) -> void {
	if ( ppu.hd() ) return plotHD( above, x, source, priority, color, false, false );
	if ( priority > above[ x ].priority ) above[ x ] = { source, priority, color };
}

auto PPU::Line::plotBelow( uint x, uint8 source, uint8 priority, uint16 color ) -> void {
	if ( ppu.hd() ) return plotHD( below, x, source, priority, color, false, false );
	if ( priority > below[ x ].priority ) below[ x ] = { source, priority, color };
}

//todo: name these variables more clearly ...
auto PPU::Line::plotHD( Pixel* pixel, uint x, uint8 source, uint8 priority, uint16 color, bool hires, bool subpixel ) -> void {
	auto scale = ppu.hdScale();
	int xss = hires && subpixel ? scale / 2 : 0;
	int ys = ppu.interlace() && field() ? scale / 2 : 0;
	if ( priority > pixel[ x * scale + xss + ys * 256 * scale ].priority ) {
		Pixel p = { source, priority, color };
		int xsm = hires && !subpixel ? scale / 2 : scale;
		int ysm = ppu.interlace() && !field() ? scale / 2 : scale;
		for ( int xs = xss; xs < xsm; xs++ ) {
			pixel[ x * scale + xs + ys * 256 * scale ] = p;
		}
		int size = sizeof( Pixel ) * ( xsm - xss );
		Pixel* source = &pixel[ x * scale + xss + ys * 256 * scale ];
		for ( int yst = ys + 1; yst < ysm; yst++ ) {
			memcpy( &pixel[ x * scale + xss + yst * 256 * scale ], source, size );
		}
	}
}

//single-threaded
auto PPU::Line::cacheBackground( PPU::IO::Background& bg ) -> void {
	if ( y == 1 ) {
		bg.mosaicCounter = ppu.io.mosaicSize + 1;
		bg.mosaicOffset = 1;
	}
	else if ( --bg.mosaicCounter == 0 ) {
		bg.mosaicCounter = ppu.io.mosaicSize + 1;
		bg.mosaicOffset += ppu.io.mosaicSize + 1;
	}
}

//parallelized
auto PPU::Line::renderBackground( PPU::IO::Background& self, uint8 source ) -> void {
	if ( !self.aboveEnable && !self.belowEnable ) return;
	if ( self.tileMode == TileMode::Mode7 ) return renderMode7( self, source );
	if ( self.tileMode == TileMode::Inactive ) return;

	bool windowAbove[ 256 ];
	bool windowBelow[ 256 ];
	renderWindow( self.window, self.window.aboveEnable, windowAbove );
	renderWindow( self.window, self.window.belowEnable, windowBelow );

	bool hires = io.bgMode == 5 || io.bgMode == 6;
	bool offsetPerTileMode = io.bgMode == 2 || io.bgMode == 4 || io.bgMode == 6;
	bool directColorMode = io.col.directColor && source == Source::BG1 && ( io.bgMode == 3 || io.bgMode == 4 );
	uint colorShift = 3 + self.tileMode;
	int width = 256 << hires;

	uint tileHeight = 3 + self.tileSize;
	uint tileWidth = !hires ? tileHeight : 4;
	uint tileMask = 0x0fff >> self.tileMode;
	uint tiledataIndex = self.tiledataAddress >> 3 + self.tileMode;

	uint paletteBase = io.bgMode == 0 ? source << 5 : 0;
	uint paletteShift = 2 << self.tileMode;

	uint hscroll = self.hoffset;
	uint vscroll = self.voffset;
	uint hmask = ( width << self.tileSize << !!( self.screenSize & 1 ) ) - 1;
	uint vmask = ( width << self.tileSize << !!( self.screenSize & 2 ) ) - 1;

	uint y = self.mosaicEnable ? self.mosaicOffset : this->y;
	if ( hires ) {
		hscroll <<= 1;
		if ( io.interlace ) y = y << 1 | field();
	}

	uint mosaicCounter = 1;
	uint mosaicPalette = 0;
	uint8 mosaicPriority = 0;
	uint16 mosaicColor = 0;

	int x = 0 - ( hscroll & 7 );
	while ( x < width ) {
		uint hoffset = x + hscroll;
		uint voffset = y + vscroll;
		if ( offsetPerTileMode ) {
			uint validBit = 0x2000 << source;
			uint offsetX = x + ( hscroll & 7 );
			if ( offsetX >= 8 ) {  //first column is exempt
				uint hlookup = getTile( io.bg3, ( offsetX - 8 ) + ( io.bg3.hoffset & ~7 ), io.bg3.voffset + 0 );
				if ( io.bgMode == 4 ) {
					if ( hlookup & validBit ) {
						if ( !( hlookup & 0x8000 ) ) {
							hoffset = offsetX + ( hlookup & ~7 );
						}
						else {
							voffset = y + hlookup;
						}
					}
				}
				else {
					uint vlookup = getTile( io.bg3, ( offsetX - 8 ) + ( io.bg3.hoffset & ~7 ), io.bg3.voffset + 8 );
					if ( hlookup & validBit ) {
						hoffset = offsetX + ( hlookup & ~7 );
					}
					if ( vlookup & validBit ) {
						voffset = y + vlookup;
					}
				}
			}
		}
		hoffset &= hmask;
		voffset &= vmask;

		uint tileNumber = getTile( self, hoffset, voffset );
		uint mirrorY = tileNumber & 0x8000 ? 7 : 0;
		uint mirrorX = tileNumber & 0x4000 ? 7 : 0;
		uint8 tilePriority = self.priority[ bool( tileNumber & 0x2000 ) ];
		uint paletteNumber = tileNumber >> 10 & 7;
		uint paletteIndex = paletteBase + ( paletteNumber << paletteShift ) & 0xff;

		if ( tileWidth == 4 && ( bool( hoffset & 8 ) ^ bool( mirrorX ) ) ) tileNumber += 1;
		if ( tileHeight == 4 && ( bool( voffset & 8 ) ^ bool( mirrorY ) ) ) tileNumber += 16;
		tileNumber = ( tileNumber & 0x03ff ) + tiledataIndex & tileMask;

		uint16 address;
		address = ( tileNumber << colorShift ) + ( voffset & 7 ^ mirrorY ) & 0x7fff;

		uint64 data;
		data = (uint64)ppu.vram[ address + 0 ] << 0;
		data |= (uint64)ppu.vram[ address + 8 ] << 16;
		data |= (uint64)ppu.vram[ address + 16 ] << 32;
		data |= (uint64)ppu.vram[ address + 24 ] << 48;

		for ( uint tileX = 0; tileX < 8; tileX++, x++ ) {
			if ( x & width ) continue;  //x < 0 || x >= width
			if ( !self.mosaicEnable || --mosaicCounter == 0 ) {
				uint color, shift = mirrorX ? tileX : 7 - tileX;
				/*if(self.tileMode >= TileMode::BPP2)*/ {
					color = data >> shift + 0 & 1;
					color += data >> shift + 7 & 2;
				}
				if ( self.tileMode >= TileMode::BPP4 ) {
					color += data >> shift + 14 & 4;
					color += data >> shift + 21 & 8;
				}
				if ( self.tileMode >= TileMode::BPP8 ) {
					color += data >> shift + 28 & 16;
					color += data >> shift + 35 & 32;
					color += data >> shift + 42 & 64;
					color += data >> shift + 49 & 128;
				}

				mosaicCounter = 1 + io.mosaicSize;
				mosaicPalette = color;
				mosaicPriority = tilePriority;
				if ( directColorMode ) {
					mosaicColor = directColor( paletteNumber, mosaicPalette );
				}
				else {
					mosaicColor = cgram[ paletteIndex + mosaicPalette ];
				}
			}
			if ( !mosaicPalette ) continue;

			if ( !hires ) {
				if ( self.aboveEnable && !windowAbove[ x ] ) plotAbove( x, source, mosaicPriority, mosaicColor );
				if ( self.belowEnable && !windowBelow[ x ] ) plotBelow( x, source, mosaicPriority, mosaicColor );
			}
			else {
				uint X = x >> 1;
				if ( !ppu.hd() ) {
					if ( x & 1 ) {
						if ( self.aboveEnable && !windowAbove[ X ] ) plotAbove( X, source, mosaicPriority, mosaicColor );
					}
					else {
						if ( self.belowEnable && !windowBelow[ X ] ) plotBelow( X, source, mosaicPriority, mosaicColor );
					}
				}
				else {
					if ( self.aboveEnable && !windowAbove[ X ] ) plotHD( above, X, source, mosaicPriority, mosaicColor, true, x & 1 );
					if ( self.belowEnable && !windowBelow[ X ] ) plotHD( below, X, source, mosaicPriority, mosaicColor, true, x & 1 );
				}
			}
		}
	}
}

auto PPU::Line::getTile( PPU::IO::Background& self, uint hoffset, uint voffset ) -> uint {
	bool hires = io.bgMode == 5 || io.bgMode == 6;
	uint tileHeight = 3 + self.tileSize;
	uint tileWidth = !hires ? tileHeight : 4;
	uint screenX = self.screenSize & 1 ? 32 << 5 : 0;
	uint screenY = self.screenSize & 2 ? 32 << 5 + ( self.screenSize & 1 ) : 0;
	uint tileX = hoffset >> tileWidth;
	uint tileY = voffset >> tileHeight;
	uint offset = ( tileY & 0x1f ) << 5 | ( tileX & 0x1f );
	if ( tileX & 0x20 ) offset += screenX;
	if ( tileY & 0x20 ) offset += screenY;
	return ppu.vram[ self.screenAddress + offset & 0x7fff ];
}

auto PPU::Line::renderMode7( PPU::IO::Background& self, uint8 source ) -> void {
	//HD mode 7 support
	if ( !ppu.hdMosaic() || !self.mosaicEnable || !io.mosaicSize ) {
		if ( ppu.hdScale() > 1 ) return renderMode7HD( self, source );
	}

	int Y = self.mosaicEnable ? self.mosaicOffset : this->y;
	int y = !io.mode7.vflip ? Y : 255 - Y;

	int a = (int16)io.mode7.a;
	int b = (int16)io.mode7.b;
	int c = (int16)io.mode7.c;
	int d = (int16)io.mode7.d;
	int hcenter = (int13)io.mode7.x;
	int vcenter = (int13)io.mode7.y;
	int hoffset = (int13)io.mode7.hoffset;
	int voffset = (int13)io.mode7.voffset;

	uint mosaicCounter = 1;
	uint mosaicPalette = 0;
	uint8 mosaicPriority = 0;
	uint16 mosaicColor = 0;

	auto clip = []( int n ) -> int { return n & 0x2000 ? ( n | ~1023 ) : ( n & 1023 ); };
	int originX = ( a * clip( hoffset - hcenter ) & ~63 ) + ( b * clip( voffset - vcenter ) & ~63 ) + ( b * y & ~63 ) + ( hcenter << 8 );
	int originY = ( c * clip( hoffset - hcenter ) & ~63 ) + ( d * clip( voffset - vcenter ) & ~63 ) + ( d * y & ~63 ) + ( vcenter << 8 );

	bool windowAbove[ 256 ];
	bool windowBelow[ 256 ];
	renderWindow( self.window, self.window.aboveEnable, windowAbove );
	renderWindow( self.window, self.window.belowEnable, windowBelow );

	for ( int X : range( 256 ) ) {
		int x = !io.mode7.hflip ? X : 255 - X;
		int pixelX = originX + a * x >> 8;
		int pixelY = originY + c * x >> 8;
		int tileX = pixelX >> 3 & 127;
		int tileY = pixelY >> 3 & 127;
		bool outOfBounds = ( pixelX | pixelY ) & ~1023;
		uint15 tileAddress = tileY * 128 + tileX;
		uint15 paletteAddress = ( ( pixelY & 7 ) << 3 ) + ( pixelX & 7 );
		uint8 tile = io.mode7.repeat == 3 && outOfBounds ? 0 : ppu.vram[ tileAddress ] >> 0;
		uint8 palette = io.mode7.repeat == 2 && outOfBounds ? 0 : ppu.vram[ tile << 6 | paletteAddress ] >> 8;

		uint8 priority;
		if ( source == Source::BG1 ) {
			priority = self.priority[ 0 ];
		}
		else if ( source == Source::BG2 ) {
			priority = self.priority[ palette >> 7 ];
			palette &= 0x7f;
		}

		if ( !self.mosaicEnable || --mosaicCounter == 0 ) {
			mosaicCounter = 1 + io.mosaicSize;
			mosaicPalette = palette;
			mosaicPriority = priority;
			if ( io.col.directColor && source == Source::BG1 ) {
				mosaicColor = directColor( 0, palette );
			}
			else {
				mosaicColor = cgram[ palette ];
			}
		}
		if ( !mosaicPalette ) continue;

		if ( self.aboveEnable && !windowAbove[ X ] ) plotAbove( X, source, mosaicPriority, mosaicColor );
		if ( self.belowEnable && !windowBelow[ X ] ) plotBelow( X, source, mosaicPriority, mosaicColor );
	}
}

//determine mode 7 line groups for perspective correction
auto PPU::Line::cacheMode7HD() -> void {
	ppu.mode7LineGroups.count = 0;
	if ( ppu.hdPerspective() ) {
#define isLineMode7(line) (line.io.bg1.tileMode == TileMode::Mode7 && !line.io.displayDisable && ( \
      (line.io.bg1.aboveEnable || line.io.bg1.belowEnable) \
    ))
		bool state = false;
		uint y;
		//find the moe 7 groups
		for ( y = 0; y < Line::count; y++ ) {
			if ( state != isLineMode7( ppu.lines[ Line::start + y ] ) ) {
				state = !state;
				if ( state ) {
					ppu.mode7LineGroups.startLine[ ppu.mode7LineGroups.count ] = ppu.lines[ Line::start + y ].y;
				}
				else {
					ppu.mode7LineGroups.endLine[ ppu.mode7LineGroups.count ] = ppu.lines[ Line::start + y ].y - 1;
					//the lines at the edges of mode 7 groups may be erroneous, so start and end lines for interpolation are moved inside
					int offset = ( ppu.mode7LineGroups.endLine[ ppu.mode7LineGroups.count ] - ppu.mode7LineGroups.startLine[ ppu.mode7LineGroups.count ] ) / 8;
					ppu.mode7LineGroups.startLerpLine[ ppu.mode7LineGroups.count ] = ppu.mode7LineGroups.startLine[ ppu.mode7LineGroups.count ] + offset;
					ppu.mode7LineGroups.endLerpLine[ ppu.mode7LineGroups.count ] = ppu.mode7LineGroups.endLine[ ppu.mode7LineGroups.count ] - offset;
					ppu.mode7LineGroups.count++;
				}
			}
		}
#undef isLineMode7
		if ( state ) {
			//close the last group if necessary
			ppu.mode7LineGroups.endLine[ ppu.mode7LineGroups.count ] = ppu.lines[ Line::start + y ].y - 1;
			int offset = ( ppu.mode7LineGroups.endLine[ ppu.mode7LineGroups.count ] - ppu.mode7LineGroups.startLine[ ppu.mode7LineGroups.count ] ) / 8;
			ppu.mode7LineGroups.startLerpLine[ ppu.mode7LineGroups.count ] = ppu.mode7LineGroups.startLine[ ppu.mode7LineGroups.count ] + offset;
			ppu.mode7LineGroups.endLerpLine[ ppu.mode7LineGroups.count ] = ppu.mode7LineGroups.endLine[ ppu.mode7LineGroups.count ] - offset;
			ppu.mode7LineGroups.count++;
		}

		//detect groups that do not have perspective
		for ( int i : range( ppu.mode7LineGroups.count ) ) {
			int a = -1, b = -1, c = -1, d = -1;  //the mode 7 scale factors of the current line
			int aPrev = -1, bPrev = -1, cPrev = -1, dPrev = -1;  //the mode 7 scale factors of the previous line
			bool aVar = false, bVar = false, cVar = false, dVar = false;  //has a varying value been found for the factors?
			bool aInc = false, bInc = false, cInc = false, dInc = false;  //has the variation been an increase or decrease?
			for ( y = ppu.mode7LineGroups.startLerpLine[ i ]; y <= ppu.mode7LineGroups.endLerpLine[ i ]; y++ ) {
				a = ( (int)( (int16)( ppu.lines[ y ].io.mode7.a ) ) );
				b = ( (int)( (int16)( ppu.lines[ y ].io.mode7.b ) ) );
				c = ( (int)( (int16)( ppu.lines[ y ].io.mode7.c ) ) );
				d = ( (int)( (int16)( ppu.lines[ y ].io.mode7.d ) ) );
				//has the value of 'a' changed compared to the last line?
				//(and is the factor larger than zero, which happens sometimes and seems to be game-specific, mostly at the edges of the screen)
				if ( aPrev > 0 && a > 0 && a != aPrev ) {
					if ( !aVar ) {
						//if there has been no variation yet, store that there is one and store if it is an increase or decrease
						aVar = true;
						aInc = a > aPrev;
					}
					else if ( aInc != a > aPrev ) {
						//if there has been an increase and now we have a decrease, or vice versa, set the interpolation lines to -1
						//to deactivate perspective correction for this group and stop analyzing it further
						ppu.mode7LineGroups.startLerpLine[ i ] = -1;
						ppu.mode7LineGroups.endLerpLine[ i ] = -1;
						break;
					}
				}
				if ( bPrev > 0 && b > 0 && b != bPrev ) {
					if ( !bVar ) {
						bVar = true;
						bInc = b > bPrev;
					}
					else if ( bInc != b > bPrev ) {
						ppu.mode7LineGroups.startLerpLine[ i ] = -1;
						ppu.mode7LineGroups.endLerpLine[ i ] = -1;
						break;
					}
				}
				if ( cPrev > 0 && c > 0 && c != cPrev ) {
					if ( !cVar ) {
						cVar = true;
						cInc = c > cPrev;
					}
					else if ( cInc != c > cPrev ) {
						ppu.mode7LineGroups.startLerpLine[ i ] = -1;
						ppu.mode7LineGroups.endLerpLine[ i ] = -1;
						break;
					}
				}
				if ( dPrev > 0 && d > 0 && d != bPrev ) {
					if ( !dVar ) {
						dVar = true;
						dInc = d > dPrev;
					}
					else if ( dInc != d > dPrev ) {
						ppu.mode7LineGroups.startLerpLine[ i ] = -1;
						ppu.mode7LineGroups.endLerpLine[ i ] = -1;
						break;
					}
				}
				aPrev = a, bPrev = b, cPrev = c, dPrev = d;
			}
		}
	}
}

auto PPU::Line::renderMode7HD( PPU::IO::Background& self, uint8 source ) -> void {
	const bool extbg = source == Source::BG2;
	const uint scale = ppu.hdScale();

	Pixel  pixel;
	Pixel* above = &this->above[ -1 ];
	Pixel* below = &this->below[ -1 ];

	//find the first and last scanline for interpolation
	int y_a = -1;
	int y_b = -1;
#define isLineMode7(n) (ppu.lines[n].io.bg1.tileMode == TileMode::Mode7 && !ppu.lines[n].io.displayDisable && ( \
    (ppu.lines[n].io.bg1.aboveEnable || ppu.lines[n].io.bg1.belowEnable) \
  ))
	if ( ppu.hdPerspective() ) {
		//find the mode 7 line group this line is in and use its interpolation lines
		for ( int i : range( ppu.mode7LineGroups.count ) ) {
			if ( y >= ppu.mode7LineGroups.startLine[ i ] && y <= ppu.mode7LineGroups.endLine[ i ] ) {
				y_a = ppu.mode7LineGroups.startLerpLine[ i ];
				y_b = ppu.mode7LineGroups.endLerpLine[ i ];
				break;
			}
		}
	}
	if ( y_a == -1 || y_b == -1 ) {
		//if perspective correction is disabled or the group was detected as non-perspective, use the neighboring lines
		y_a = y;
		y_b = y;
		if ( y_a > 1 && isLineMode7( y_a ) ) y_a--;
		if ( y_b < 239 && isLineMode7( y_b ) ) y_b++;
	}
#undef isLineMode7

	Line line_a = ppu.lines[ y_a ];
	float a_a = (int16)line_a.io.mode7.a;
	float b_a = (int16)line_a.io.mode7.b;
	float c_a = (int16)line_a.io.mode7.c;
	float d_a = (int16)line_a.io.mode7.d;

	Line line_b = ppu.lines[ y_b ];
	float a_b = (int16)line_b.io.mode7.a;
	float b_b = (int16)line_b.io.mode7.b;
	float c_b = (int16)line_b.io.mode7.c;
	float d_b = (int16)line_b.io.mode7.d;

	int hcenter = (int13)io.mode7.x;
	int vcenter = (int13)io.mode7.y;
	int hoffset = (int13)io.mode7.hoffset;
	int voffset = (int13)io.mode7.voffset;

	if ( io.mode7.vflip ) {
		y_a = 255 - y_a;
		y_b = 255 - y_b;
	}

	bool windowAbove[ 256 ];
	bool windowBelow[ 256 ];
	renderWindow( self.window, self.window.aboveEnable, windowAbove );
	renderWindow( self.window, self.window.belowEnable, windowBelow );

	int pixelYp = INT_MIN;
	for ( int ys : range( scale ) ) {
		float yf = y + ys * 1.0 / scale - 0.5;
		if ( io.mode7.vflip ) yf = 255 - yf;

		float a = 1.0 / lerp( y_a, 1.0 / a_a, y_b, 1.0 / a_b, yf );
		float b = 1.0 / lerp( y_a, 1.0 / b_a, y_b, 1.0 / b_b, yf );
		float c = 1.0 / lerp( y_a, 1.0 / c_a, y_b, 1.0 / c_b, yf );
		float d = 1.0 / lerp( y_a, 1.0 / d_a, y_b, 1.0 / d_b, yf );

		int ht = ( hoffset - hcenter ) % 1024;
		float vty = ( ( voffset - vcenter ) % 1024 ) + yf;
		float originX = ( a * ht ) + ( b * vty ) + ( hcenter << 8 );
		float originY = ( c * ht ) + ( d * vty ) + ( vcenter << 8 );

		int pixelXp = INT_MIN;
		for ( int x : range( 256 ) ) {
			bool doAbove = self.aboveEnable && !windowAbove[ x ];
			bool doBelow = self.belowEnable && !windowBelow[ x ];

			for ( int xs : range( scale ) ) {
				float xf = x + xs * 1.0 / scale - 0.5;
				if ( io.mode7.hflip ) xf = 255 - xf;

				int pixelX = ( originX + a * xf ) / 256;
				int pixelY = ( originY + c * xf ) / 256;

				above++;
				below++;

				//only compute color again when coordinates have changed
				if ( pixelX != pixelXp || pixelY != pixelYp ) {
					uint tile = io.mode7.repeat == 3 && ( ( pixelX | pixelY ) & ~1023 ) ? 0 : ( ppu.vram[ ( pixelY >> 3 & 127 ) * 128 + ( pixelX >> 3 & 127 ) ] & 0xff );
					uint palette = io.mode7.repeat == 2 && ( ( pixelX | pixelY ) & ~1023 ) ? 0 : ( ppu.vram[ ( ( ( pixelY & 7 ) << 3 ) + ( pixelX & 7 ) ) + ( tile << 6 ) ] >> 8 );

					uint8 priority;
					if ( !extbg ) {
						priority = self.priority[ 0 ];
					}
					else {
						priority = self.priority[ palette >> 7 ];
						palette &= 0x7f;
					}
					if ( !palette ) continue;

					uint16 color;
					if ( io.col.directColor && !extbg ) {
						color = directColor( 0, palette );
					}
					else {
						color = cgram[ palette ];
					}

					pixel = { source, priority, color };
					pixelXp = pixelX;
					pixelYp = pixelY;
				}

				if ( doAbove && ( !extbg || pixel.priority > above->priority ) ) *above = pixel;
				if ( doBelow && ( !extbg || pixel.priority > below->priority ) ) *below = pixel;
			}
		}
	}

	if ( ppu.ss() ) {
		uint divisor = scale * scale;
		for ( uint p : range( 256 ) ) {
			uint ab = 0, bb = 0;
			uint ag = 0, bg = 0;
			uint ar = 0, br = 0;
			for ( uint y : range( scale ) ) {
				auto above = &this->above[ p * scale ];
				auto below = &this->below[ p * scale ];
				for ( uint x : range( scale ) ) {
					uint a = above[ x ].color;
					uint b = below[ x ].color;
					ab += a >> 0 & 31;
					ag += a >> 5 & 31;
					ar += a >> 10 & 31;
					bb += b >> 0 & 31;
					bg += b >> 5 & 31;
					br += b >> 10 & 31;
				}
			}
			uint16 aboveColor = ab / divisor << 0 | ag / divisor << 5 | ar / divisor << 10;
			uint16 belowColor = bb / divisor << 0 | bg / divisor << 5 | br / divisor << 10;
			this->above[ p ] = { source, this->above[ p * scale ].priority, aboveColor };
			this->below[ p ] = { source, this->below[ p * scale ].priority, belowColor };
		}
	}
}

//interpolation and extrapolation
auto PPU::Line::lerp( float pa, float va, float pb, float vb, float pr ) -> float {
	if ( va == vb || pr == pa ) return va;
	if ( pr == pb ) return vb;
	return va + ( vb - va ) / ( pb - pa ) * ( pr - pa );
}

auto PPU::Line::renderObject( PPU::IO::Object& self ) -> void {
	if ( !self.aboveEnable && !self.belowEnable ) return;

	bool windowAbove[ 256 ];
	bool windowBelow[ 256 ];
	renderWindow( self.window, self.window.aboveEnable, windowAbove );
	renderWindow( self.window, self.window.belowEnable, windowBelow );

	uint itemCount = 0;
	uint tileCount = 0;
	for ( uint n : range( ppu.ItemLimit ) ) items[ n ].valid = false;
	for ( uint n : range( ppu.TileLimit ) ) tiles[ n ].valid = false;

	for ( uint n : range( 128 ) ) {
		ObjectItem item{ true, uint8_t( self.first + n & 127 ) };
		const auto& object = ppu.objects[ item.index ];

		if ( object.size == 0 ) {
			static const uint widths[] = { 8,  8,  8, 16, 16, 32, 16, 16 };
			static const uint heights[] = { 8,  8,  8, 16, 16, 32, 32, 32 };
			item.width = widths[ self.baseSize ];
			item.height = heights[ self.baseSize ];
			if ( self.interlace && self.baseSize >= 6 ) item.height = 16;  //hardware quirk
		}
		else {
			static const uint widths[] = { 16, 32, 64, 32, 64, 64, 32, 32 };
			static const uint heights[] = { 16, 32, 64, 32, 64, 64, 64, 32 };
			item.width = widths[ self.baseSize ];
			item.height = heights[ self.baseSize ];
		}

		if ( object.x > 256 && object.x + item.width - 1 < 512 ) continue;
		uint height = item.height >> self.interlace;
		if ( ( y >= object.y && y < object.y + height )
			|| ( object.y + height >= 256 && y < ( object.y + height & 255 ) )
			) {
			if ( itemCount++ >= ppu.ItemLimit ) break;
			items[ itemCount - 1 ] = item;
		}
	}

	for ( int n = ppu.ItemLimit - 1; n >= 0; n--) {
		const auto& item = items[ n ];
		if ( !item.valid ) continue;

		const auto& object = ppu.objects[ item.index ];
		uint tileWidth = item.width >> 3;
		int x = object.x;
		int y = this->y - object.y & 0xff;
		if ( self.interlace ) y <<= 1;

		if ( object.vflip ) {
			if ( item.width == item.height ) {
				y = item.height - 1 - y;
			}
			else if ( y < item.width ) {
				y = item.width - 1 - y;
			}
			else {
				y = item.width + ( item.width - 1 ) - ( y - item.width );
			}
		}

		if ( self.interlace ) {
			y = !object.vflip ? y + field() : y - field();
		}

		x &= 511;
		y &= 255;

		uint16 tiledataAddress = self.tiledataAddress;
		if ( object.nameselect ) tiledataAddress += 1 + self.nameselect << 12;
		uint16 characterX = ( object.character & 15 );
		uint16 characterY = ( ( object.character >> 4 ) + ( y >> 3 ) & 15 ) << 4;

		for ( uint tileX : range( tileWidth ) ) {
			uint objectX = x + ( tileX << 3 ) & 511;
			if ( x != 256 && objectX >= 256 && objectX + 7 < 512 ) continue;

			ObjectTile tile{ true };
			tile.x = objectX;
			tile.y = y;
			tile.priority = object.priority;
			tile.palette = 128 + ( object.palette << 4 );
			tile.hflip = object.hflip;

			uint mirrorX = !object.hflip ? tileX : tileWidth - 1 - tileX;
			uint address = tiledataAddress + ( ( characterY + ( characterX + mirrorX & 15 ) ) << 4 );
			address = ( address & 0x7ff0 ) + ( y & 7 );
			tile.data = ppu.vram[ address + 0 ] << 0;
			tile.data |= ppu.vram[ address + 8 ] << 16;

			if ( tileCount++ >= ppu.TileLimit ) break;
			tiles[ tileCount - 1 ] = tile;
		}
	}

	ppu.io.obj.rangeOver |= itemCount > ppu.ItemLimit;
	ppu.io.obj.timeOver |= tileCount > ppu.TileLimit;

	uint8_t palette[ 256 ] = {};
	uint8_t priority[ 256 ] = {};

	for ( uint n : range( ppu.TileLimit ) ) {
		auto& tile = tiles[ n ];
		if ( !tile.valid ) continue;

		uint tileX = tile.x;
		for ( uint x : range( 8 ) ) {
			tileX &= 511;
			if ( tileX < 256 ) {
				uint color, shift = tile.hflip ? x : 7 - x;
				color = tile.data >> shift + 0 & 1;
				color += tile.data >> shift + 7 & 2;
				color += tile.data >> shift + 14 & 4;
				color += tile.data >> shift + 21 & 8;
				if ( color ) {
					palette[ tileX ] = tile.palette + color;
					priority[ tileX ] = self.priority[ tile.priority ];
				}
			}
			tileX++;
		}
	}

	for ( uint x : range( 256 ) ) {
		if ( !priority[ x ] ) continue;
		uint8 source = palette[ x ] < 192 ? Source::OBJ1 : Source::OBJ2;
		if ( self.aboveEnable && !windowAbove[ x ] ) plotAbove( x, source, priority[ x ], cgram[ palette[ x ] ] );
		if ( self.belowEnable && !windowBelow[ x ] ) plotBelow( x, source, priority[ x ], cgram[ palette[ x ] ] );
	}
}

auto PPU::oamAddressReset() -> void {
	io.oamAddress = io.oamBaseAddress;
	oamSetFirstObject();
}

auto PPU::oamSetFirstObject() -> void {
	io.obj.first = !io.oamPriority ? 0 : io.oamAddress >> 2 & 0x7f;
}

auto PPU::readObject( uint10 address ) -> uint8 {
	if ( !( address & 0x200 ) ) {
		uint n = address >> 2;  //object#
		address &= 3;
		if ( address == 0 ) return objects[ n ].x;
		if ( address == 1 ) return objects[ n ].y - 1;
		if ( address == 2 ) return objects[ n ].character;
		return (
			objects[ n ].nameselect << 0
			| objects[ n ].palette << 1
			| objects[ n ].priority << 4
			| objects[ n ].hflip << 6
			| objects[ n ].vflip << 7
			);
	}
	else {
		uint n = ( address & 0x1f ) << 2;  //object#
		return (
			objects[ n + 0 ].x >> 8 << 0
			| objects[ n + 0 ].size << 1
			| objects[ n + 1 ].x >> 8 << 2
			| objects[ n + 1 ].size << 3
			| objects[ n + 2 ].x >> 8 << 4
			| objects[ n + 2 ].size << 5
			| objects[ n + 3 ].x >> 8 << 6
			| objects[ n + 3 ].size << 7
			);
	}
}

auto PPU::writeObject( uint10 address, uint8 data ) -> void {
	if ( !( address & 0x200 ) ) {
		uint n = address >> 2;  //object#
		address &= 3;
		if ( address == 0 ) { objects[ n ].x = objects[ n ].x & 0x100 | data; return; }
		if ( address == 1 ) { objects[ n ].y = data + 1; return; }  //+1 => rendering happens one scanline late
		if ( address == 2 ) { objects[ n ].character = data; return; }
		objects[ n ].nameselect = data >> 0 & 1;
		objects[ n ].palette = data >> 1 & 7;
		objects[ n ].priority = data >> 4 & 3;
		objects[ n ].hflip = data >> 6 & 1;
		objects[ n ].vflip = data >> 7 & 1;
	}
	else {
		uint n = ( address & 0x1f ) << 2;  //object#
		objects[ n + 0 ].x = objects[ n + 0 ].x & 0xff | data << 8 & 0x100;
		objects[ n + 1 ].x = objects[ n + 1 ].x & 0xff | data << 6 & 0x100;
		objects[ n + 2 ].x = objects[ n + 2 ].x & 0xff | data << 4 & 0x100;
		objects[ n + 3 ].x = objects[ n + 3 ].x & 0xff | data << 2 & 0x100;
		objects[ n + 0 ].size = data >> 1 & 1;
		objects[ n + 1 ].size = data >> 3 & 1;
		objects[ n + 2 ].size = data >> 5 & 1;
		objects[ n + 3 ].size = data >> 7 & 1;
	}
}

auto PPU::Line::renderWindow( PPU::IO::WindowLayer& self, bool enable, bool output[ 256 ] ) -> void {
	if ( !enable || ( !self.oneEnable && !self.twoEnable ) ) {
		memory::fill<bool>( output, 256, 0 );
		return;
	}

	if ( self.oneEnable && !self.twoEnable ) {
		bool set = 1 ^ self.oneInvert, clear = !set;
		for ( uint x : range( 256 ) ) {
			output[ x ] = x >= io.window.oneLeft && x <= io.window.oneRight ? set : clear;
		}
		return;
	}

	if ( self.twoEnable && !self.oneEnable ) {
		bool set = 1 ^ self.twoInvert, clear = !set;
		for ( uint x : range( 256 ) ) {
			output[ x ] = x >= io.window.twoLeft && x <= io.window.twoRight ? set : clear;
		}
		return;
	}

	for ( uint x : range( 256 ) ) {
		bool oneMask = ( x >= io.window.oneLeft && x <= io.window.oneRight ) ^ self.oneInvert;
		bool twoMask = ( x >= io.window.twoLeft && x <= io.window.twoRight ) ^ self.twoInvert;
		switch ( self.mask ) {
		case 0: output[ x ] = ( oneMask | twoMask ) == 1; break;
		case 1: output[ x ] = ( oneMask & twoMask ) == 1; break;
		case 2: output[ x ] = ( oneMask ^ twoMask ) == 1; break;
		case 3: output[ x ] = ( oneMask ^ twoMask ) == 0; break;
		}
	}
}

auto PPU::Line::renderWindow( PPU::IO::WindowColor& self, uint mask, bool output[ 256 ] ) -> void {
	bool set, clear;
	switch ( mask ) {
	case 0: memory::fill<bool>( output, 256, 1 ); return;  //always
	case 1: set = 1, clear = 0; break;  //inside
	case 2: set = 0, clear = 1; break;  //outside
	case 3: memory::fill<bool>( output, 256, 0 ); return;  //never
	}

	if ( !self.oneEnable && !self.twoEnable ) {
		memory::fill<bool>( output, 256, clear );
		return;
	}

	if ( self.oneEnable && !self.twoEnable ) {
		if ( self.oneInvert ) set ^= 1, clear ^= 1;
		for ( uint x : range( 256 ) ) {
			output[ x ] = x >= io.window.oneLeft && x <= io.window.oneRight ? set : clear;
		}
		return;
	}

	if ( self.twoEnable && !self.oneEnable ) {
		if ( self.twoInvert ) set ^= 1, clear ^= 1;
		for ( uint x : range( 256 ) ) {
			output[ x ] = x >= io.window.twoLeft && x <= io.window.twoRight ? set : clear;
		}
		return;
	}

	for ( uint x : range( 256 ) ) {
		bool oneMask = ( x >= io.window.oneLeft && x <= io.window.oneRight ) ^ self.oneInvert;
		bool twoMask = ( x >= io.window.twoLeft && x <= io.window.twoRight ) ^ self.twoInvert;
		switch ( self.mask ) {
		case 0: output[ x ] = ( oneMask | twoMask ) == 1 ? set : clear; break;
		case 1: output[ x ] = ( oneMask & twoMask ) == 1 ? set : clear; break;
		case 2: output[ x ] = ( oneMask ^ twoMask ) == 1 ? set : clear; break;
		case 3: output[ x ] = ( oneMask ^ twoMask ) == 0 ? set : clear; break;
		}
	}
}


auto PPU::interlace() const -> bool { return false;/*return ppubase.display.interlace;*/ }
auto PPU::overscan() const -> bool { return false;/*return ppubase.display.overscan;*/ }
auto PPU::vdisp() const -> uint { return 0;/*return ppubase.display.vdisp;*/ }
auto PPU::hires() const -> bool { return latch.hires; }
auto PPU::hd() const -> bool { return latch.hd; }
auto PPU::ss() const -> bool { return latch.ss; }
#undef ppu
auto PPU::hdScale() const -> uint { return 1;/*return configuration.hacks.ppu.mode7.scale;*/ }
auto PPU::hdPerspective() const -> bool { return true;/*return configuration.hacks.ppu.mode7.perspective;*/ }
auto PPU::hdSupersample() const -> bool { return true;/*return configuration.hacks.ppu.mode7.supersample;*/ }
auto PPU::hdMosaic() const -> bool { return false;/*return configuration.hacks.ppu.mode7.mosaic;*/ }
auto PPU::deinterlace() const -> bool { return false;/*return configuration.hacks.ppu.deinterlace;*/ }
auto PPU::renderCycle() const -> uint { return 0;/*return configuration.hacks.ppu.renderCycle;*/ }
auto PPU::noVRAMBlocking() const -> bool { return true;/*return configuration.hacks.ppu.noVRAMBlocking;*/ }
#define ppu ppufast

PPU::PPU() {
  output = new uint16_t[2304 * 2160]();

  for(uint l : range(16)) {
    lightTable[l] = new uint16_t[32768];
    for(uint r : range(32)) {
      for(uint g : range(32)) {
        for(uint b : range(32)) {
          double luma = (double)l / 15.0;
          uint ar = (luma * r + 0.5);
          uint ag = (luma * g + 0.5);
          uint ab = (luma * b + 0.5);
          lightTable[l][r << 10 | g << 5 | b << 0] = ab << 10 | ag << 5 | ar << 0;
        }
      }
    }
  }

  for(uint y : range(240)) {
    lines[y].y = y;
  }
}

PPU::~PPU() {
  delete[] output;
  for(uint l : range(16)) delete[] lightTable[l];
}

auto PPU::synchronizeCPU() -> void {
  //if(ppubase.clock >= 0) scheduler.resume(cpu.thread);
}

auto PPU::Enter() -> void {
  while(true) {
    //scheduler.synchronize();
    ppu.main();
  }
}

auto PPU::step(uint clocks) -> void {
  /*tick(clocks);
  ppubase.clock += clocks;*/
  synchronizeCPU();
}

auto PPU::main() -> void {
  scanline();

  /*if(system.frameCounter == 0 && !system.runAhead) {
    uint y = vcounter();
    if(y >= 1 && y <= 239) {
      step(renderCycle());
      lines[y].cache();
    }
  }*/

  //step(hperiod() - hcounter());
}

extern "C"
{
	void FUNC_80FF8A();
}

void PPU::doFrame( DmaController& dmaController, const InternalRegisterState& registerState, SDL_Window* window )
{
	dmaController.InitHDMAChannels();
	for ( uint lineIndex = 0; lineIndex < 262; lineIndex++ )
	{
		dmaController.ProcessHDMAChannels();
		if ( registerState.enableVerticalIrq && registerState.verticalTimer == lineIndex )
		{
			Line::flush();
			FUNC_80FF8A();
		}
		if ( lineIndex < 225 )
		{
			scanline();
			lines[ lineIndex ].cache();
		}
	}

	Line::flush();

	if ( window )
	{
		refresh( window );
	}

#ifdef __EMSCRIPTEN__
	emscripten_sleep(1);
#endif // __EMSCRIPTEN__

	std::cout << "PPU::doFrame" << std::endl;
}

auto PPU::scanline() -> void {
  /*if(vcounter() == 0)*/ {
    /*ppubase.display.interlace = io.interlace;
    ppubase.display.overscan = io.overscan;*/
    latch.overscan = io.overscan;
    latch.hires = false;
    latch.hd = false;
    latch.ss = false;
    io.obj.timeOver = false;
    io.obj.rangeOver = false;
  }

  ///*if(vcounter() > 0 && vcounter() < vdisp())*/ {
  //  latch.hires |= io.pseudoHires || io.bgMode == 5 || io.bgMode == 6;
  //  //supersampling and EXTBG mode are not compatible, so disable supersampling in EXTBG mode
  //  latch.hd |= io.bgMode == 7 && hdScale() > 1 && (hdSupersample() == 0 || io.extbg == 1);
  //  latch.ss |= io.bgMode == 7 && hdScale() > 1 && (hdSupersample() == 1 && io.extbg == 0);
  //}

  /*if(vcounter() == vdisp())*/ {
    //if(!io.displayDisable) oamAddressReset();
  }

  /*if(vcounter() == 240)*/ {
    //Line::flush();
  }
}

#ifdef __EMSCRIPTEN__
static std::string OpenGLOutputVertexShader = R"(#version 300 es

  uniform vec4 targetSize;
  uniform vec4 outputSize;

  in vec2 inTexCoord;
	out vec2 texCoord;

  void main() {
    //center image within output window
    if(gl_VertexID == 0 || gl_VertexID == 2) {
      gl_Position.x = -(targetSize.x / outputSize.x);
    } else {
      gl_Position.x = +(targetSize.x / outputSize.x);
    }

    //center and flip vertically (buffer[0, 0] = top-left; OpenGL[0, 0] = bottom-left)
    if(gl_VertexID == 0 || gl_VertexID == 1) {
      gl_Position.y = +(targetSize.y / outputSize.y);
    } else {
      gl_Position.y = -(targetSize.y / outputSize.y);
    }

    //align image to even pixel boundary to prevent aliasing
    vec2 align = fract((outputSize.xy + targetSize.xy) / 2.0) * 2.0;
    gl_Position.xy -= align / outputSize.xy;
    gl_Position.zw = vec2(0.0, 1.0);

    texCoord = inTexCoord;
  }
)";

static std::string OpenGLFragmentShader = R"(#version 300 es
	precision highp float;
  uniform sampler2D source;

  in vec2 texCoord;
  out vec4 fragColor;

  void main() {
    fragColor = texture(source, texCoord).bgra;
  }
)";
#else
static std::string OpenGLOutputVertexShader = R"(
#version 150

  uniform vec4 targetSize;
  uniform vec4 outputSize;

  in vec2 texCoord;

  out Vertex {
    vec2 texCoord;
  } vertexOut;

  void main() {
    //center image within output window
    if(gl_VertexID == 0 || gl_VertexID == 2) {
      gl_Position.x = -(targetSize.x / outputSize.x);
    } else {
      gl_Position.x = +(targetSize.x / outputSize.x);
    }

    //center and flip vertically (buffer[0, 0] = top-left; OpenGL[0, 0] = bottom-left)
    if(gl_VertexID == 0 || gl_VertexID == 1) {
      gl_Position.y = +(targetSize.y / outputSize.y);
    } else {
      gl_Position.y = -(targetSize.y / outputSize.y);
    }

    //align image to even pixel boundary to prevent aliasing
    vec2 align = fract((outputSize.xy + targetSize.xy) / 2.0) * 2.0;
    gl_Position.xy -= align / outputSize.xy;
    gl_Position.zw = vec2(0.0, 1.0);

    vertexOut.texCoord = texCoord;
  }
)";

static std::string OpenGLFragmentShader = R"(
  #version 150

  uniform sampler2D source;

  in Vertex {
    vec2 texCoord;
  };

  out vec4 fragColor;

  void main() {
    fragColor = texture(source, texCoord);
  }
)";
#endif // __EMSCRIPTEN__

static auto glrCreateShader( GLuint program, GLuint type, const char* source ) -> GLuint {
	GLuint shader = glCreateShader( type );
	glShaderSource( shader, 1, &source, 0 );
	glCompileShader( shader );
	GLint result = GL_FALSE;
	glGetShaderiv( shader, GL_COMPILE_STATUS, &result );
	if ( result == GL_FALSE ) {
		std::cout << "Failed to compile " << (type == GL_VERTEX_SHADER ? "vertex shader" : "fragment shader") << std::endl;
		
		GLint length = 0;
		glGetShaderiv( shader, GL_INFO_LOG_LENGTH, &length );
		std::string text( static_cast<size_t>(length + 1), '-' );
		glGetShaderInfoLog( shader, length, &length, text.data() );
		text[ length ] = 0;
		std::cout << text << std::endl;
		return 0;
	}
	glAttachShader( program, shader );
	return shader;
}

static auto glrLinkProgram( GLuint program ) -> void {
	glLinkProgram( program );
	GLint result = GL_FALSE;
	glGetProgramiv( program, GL_LINK_STATUS, &result );
	if ( result == GL_FALSE ) {
		std::cout << "Failed to link program" << std::endl;
		GLint length = 0;
		glGetProgramiv( program, GL_INFO_LOG_LENGTH, &length );
		std::string text( static_cast<size_t>( length + 1 ), '-' );
		glGetProgramInfoLog( program, length, &length, text.data() );
		text[ length ] = 0;
		std::cout << text << std::endl;
	}
	glValidateProgram( program );
	result = GL_FALSE;
	glGetProgramiv( program, GL_VALIDATE_STATUS, &result );
	if ( result == GL_FALSE ) {
		std::cout << "Failed to validate program" << std::endl;
		GLint length = 0;
		glGetProgramiv( program, GL_INFO_LOG_LENGTH, &length );
		std::string text( static_cast<size_t>( length + 1 ), '-' );
		glGetProgramInfoLog( program, length, &length, text.data() );
		text[ length ] = 0;
		std::cout << text << std::endl;
	}
}

static GLuint program = 0;
static GLuint vertex = 0;
static GLuint fragment = 0;
static GLuint vao = 0;
static GLuint vbo[ 3 ] = { 0, 0, 0 };
static GLuint texture = 0;

template<typename T> inline auto MatrixMultiply(
	T* output,
	const T* xdata, uint xrows, uint xcols,
	const T* ydata, uint yrows, uint ycols
) -> void {
	if ( xcols != yrows ) return;

	for ( uint y : range( xrows ) ) {
		for ( uint x : range( ycols ) ) {
			T sum = 0;
			for ( uint z : range( xcols ) ) {
				sum += xdata[ y * xcols + z ] * ydata[ z * ycols + x ];
			}
			*output++ = sum;
		}
	}
}

auto render( uint sourceWidth, uint sourceHeight, uint targetX, uint targetY, uint targetWidth, uint targetHeight ) -> void {
	glViewport( targetX, targetY, targetWidth, targetHeight );

	float w = (float)sourceWidth / (float)sourceWidth;
	float h = (float)sourceHeight / (float)sourceHeight;

	float u = (float)targetWidth;
	float v = (float)targetHeight;

	GLfloat modelView[] = {
		1, 0, 0, 0,
		0, 1, 0, 0,
		0, 0, 1, 0,
		0, 0, 0, 1,
	};

	GLfloat projection[] = {
		 2.0f / u,  0.0f,    0.0f, 0.0f,
		 0.0f,    2.0f / v,  0.0f, 0.0f,
		 0.0f,    0.0f,   -1.0f, 0.0f,
		-1.0f,   -1.0f,    0.0f, 1.0f,
	};

	GLfloat modelViewProjection[ 4 * 4 ];
	MatrixMultiply( modelViewProjection, modelView, 4, 4, projection, 4, 4 );

	GLfloat vertices[] = {
		0, 0, 0, 1,
		u, 0, 0, 1,
		0, v, 0, 1,
		u, v, 0, 1,
	};

	GLfloat positions[ 4 * 4 ];
	for ( uint n = 0; n < 16; n += 4 ) {
		MatrixMultiply( &positions[ n ], &vertices[ n ], 1, 4, modelViewProjection, 4, 4 );
	}

	GLfloat texCoords[] = {
		0, 0,
		w, 0,
		0, h,
		w, h,
	};

	GLint location = glGetUniformLocation( program, "modelView" );
	glUniformMatrix4fv( location, 1, GL_FALSE, modelView );

	location = glGetUniformLocation( program, "projection" );
	glUniformMatrix4fv( location, 1, GL_FALSE, projection );

	location = glGetUniformLocation( program, "modelViewProjection" );
	glUniformMatrix4fv( location, 1, GL_FALSE, modelViewProjection );

	glBindVertexArray( vao );

	/*glBindBuffer( GL_ARRAY_BUFFER, vbo[ 0 ] );
	glBufferData( GL_ARRAY_BUFFER, 16 * sizeof( GLfloat ), vertices, GL_STATIC_DRAW );
	GLint locationVertex = glGetAttribLocation( program, "vertex" );
	glEnableVertexAttribArray( locationVertex );
	glVertexAttribPointer( locationVertex, 4, GL_FLOAT, GL_FALSE, 0, 0 );

	glBindBuffer( GL_ARRAY_BUFFER, vbo[ 1 ] );
	glBufferData( GL_ARRAY_BUFFER, 16 * sizeof( GLfloat ), positions, GL_STATIC_DRAW );
	GLint locationPosition = glGetAttribLocation( program, "position" );
	glEnableVertexAttribArray( locationPosition );
	glVertexAttribPointer( locationPosition, 4, GL_FLOAT, GL_FALSE, 0, 0 );*/

	glBindBuffer( GL_ARRAY_BUFFER, vbo[ 2 ] );
	glBufferData( GL_ARRAY_BUFFER, 8 * sizeof( GLfloat ), texCoords, GL_STATIC_DRAW );
#ifdef __EMSCRIPTEN__
	GLint locationTexCoord = glGetAttribLocation( program, "inTexCoord" );
#else
	GLint locationTexCoord = glGetAttribLocation( program, "texCoord" );
#endif // __EMSCRIPTEN__
	glEnableVertexAttribArray( locationTexCoord );
	glVertexAttribPointer( locationTexCoord, 2, GL_FLOAT, GL_FALSE, 0, 0 );

#ifdef glBindFragDataLocation
	glBindFragDataLocation( program, 0, "fragColor" );
#endif // glBindFragDataLocation
	glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );

	/*glDisableVertexAttribArray( locationVertex );
	glDisableVertexAttribArray( locationPosition );*/
	glDisableVertexAttribArray( locationTexCoord );
}

void filterRender( uint32_t* colortable, uint32_t* output, uint outpitch, const uint16_t* input, uint pitch, uint width, uint height )
{
	pitch >>= 1;
	outpitch >>= 2;

	for ( uint y = 0; y < height; y++ )
	{
		const uint16_t* in = input + y * pitch;
		uint32_t* out = output + y * outpitch;
		for ( uint x = 0; x < width; x++ )
		{
			*out++ = colortable[ *in++ ];
		}
	}
}

uint32_t palette[ 32768 ];
uint32_t filteredOutput[ 256 * 240 ];

auto PPU::refresh( SDL_Window* window ) -> void {
  /*if(system.frameCounter == 0 && !system.runAhead)*/ {
    auto output = this->output;
    uint pitch, width, height;
    if(!hd()) {
      pitch  = 512 << !interlace();
      width  = 256 << hires();
      height = 240 << interlace();
    } else {
      pitch  = 256 * hdScale();
      width  = 256 * hdScale();
      height = 240 * hdScale();
    }

    //clear the areas of the screen that won't be rendered:
    //previous video frames may have drawn data here that would now be stale otherwise.
    if(!latch.overscan && pitch != frame.pitch && width != frame.width && height != frame.height) {
      for(uint y : range(240)) {
        if(y >= 8 && y <= 230) continue;  //these scanlines are always rendered.
        auto output = this->output + (!hd() ? (y * 1024 + (interlace() /*&& field()*/ ? 512 : 0)) : (y * 256 * hdScale() * hdScale()));
        auto width = (!hd() ? (!hires() ? 256 : 512) : (256 * hdScale() * hdScale()));
        memory::fill<uint16>(output, width);
      }
    }

    /*if(auto device = controllerPort2.device) device->draw(output, pitch * sizeof(uint16), width, height);
    platform->videoFrame(output, pitch * sizeof(uint16), width, height, hd() ? hdScale() : 1);*/
		//auto filteredOutput = new uint32_t[ width * height ];
		auto length = width * sizeof( uint32_t );
		filterRender( palette, filteredOutput, length, (const uint16_t*)output, (pitch * sizeof( uint16 )), width, height );

		glBindTexture( GL_TEXTURE_2D, texture );
#ifndef __EMSCRIPTEN__ 
		GLint const Swizzle[] = { GL_BLUE, GL_GREEN, GL_RED, GL_ALPHA };
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, Swizzle[ 0 ] );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, Swizzle[ 1 ] );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, Swizzle[ 2 ] );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, Swizzle[ 3 ] );
#endif // __EMSCRIPTEN__
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, filteredOutput );
		//glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, filteredOutput );

		glUseProgram( 0 );
		glBindFramebuffer( GL_DRAW_FRAMEBUFFER, 0 );
		glClearColor( 1, 1, 0, 1 );
		glClear( GL_COLOR_BUFFER_BIT );

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, texture );
		//glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, getFormat(), getType(), buffer );

		const uint32_t sourceWidth = 1600;
		const uint32_t sourceHeight = 960;
		const uint32_t outputX = 0;
		const uint32_t outputY = 0;
		const uint32_t outputWidth = 1600;
		const uint32_t outputHeight = 960;
		

		uint targetWidth = outputWidth;
		uint targetHeight = outputHeight;
		/*if ( relativeWidth ) targetWidth = sources[ 0 ].width * relativeWidth;
		if ( relativeHeight ) targetHeight = sources[ 0 ].height * relativeHeight;*/

		glUseProgram( program );

		GLint location = glGetUniformLocation( program, "source" );
		glUniform1i( location, 0 );

		location = glGetUniformLocation( program, "targetSize" );
		glUniform4f( location, targetWidth, targetHeight, 1.0 / targetWidth, 1.0 / targetHeight );

		location = glGetUniformLocation( program, "outputSize" );
		glUniform4f( location, outputWidth, outputHeight, 1.0 / outputWidth, 1.0 / outputHeight );

		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
#ifndef __EMSCRIPTEN__ 
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER );
#endif // __EMSCRIPTEN__ 

		render( sourceWidth, sourceHeight, outputX, outputY, outputWidth, outputHeight );
		
		SDL_GL_SwapWindow( window );

    frame.pitch  = pitch;
    frame.width  = width;
    frame.height = height;
  }
  //if(system.frameCounter++ >= system.frameSkip) system.frameCounter = 0;
}

auto PPU::load() -> bool {
  return true;
}

auto updateVideoPalette() -> void {
	double luminance = 100.0 / 100.0;
	double saturation = 100.0 / 100.0;
	double gamma = 100.0 / 100.0;

	uint depth = 24;

	for ( uint color : range( 32768 ) ) {
		uint16 r = ( color >> 10 ) & 31;
		uint16 g = ( color >> 5 ) & 31;
		uint16 b = ( color >> 0 ) & 31;

		r = r << 3 | r >> 2; r = r << 8 | r << 0;
		g = g << 3 | g >> 2; g = g << 8 | g << 0;
		b = b << 3 | b >> 2; b = b << 8 | b << 0;

		/*if ( saturation != 1.0 ) {
			uint16 grayscale = uclamp<16>( ( r + g + b ) / 3 );
			double inverse = max( 0.0, 1.0 - saturation );
			r = uclamp<16>( r * saturation + grayscale * inverse );
			g = uclamp<16>( g * saturation + grayscale * inverse );
			b = uclamp<16>( b * saturation + grayscale * inverse );
		}*/

		if ( gamma != 1.0 ) {
			double reciprocal = 1.0 / 32767.0;
			r = r > 32767 ? r : uint16( 32767 * pow( r * reciprocal, gamma ) );
			g = g > 32767 ? g : uint16( 32767 * pow( g * reciprocal, gamma ) );
			b = b > 32767 ? b : uint16( 32767 * pow( b * reciprocal, gamma ) );
		}

		/*if ( luminance != 1.0 ) {
			r = uclamp<16>( r * luminance );
			g = uclamp<16>( g * luminance );
			b = uclamp<16>( b * luminance );
		}*/

		switch ( depth ) {
		case 24: palette[ color ] = r >> 8 << 16 | g >> 8 << 8 | b >> 8 << 0; break;
		case 30: palette[ color ] = r >> 6 << 20 | g >> 6 << 10 | b >> 6 << 0; break;
		}

		r >>= 1;
		g >>= 1;
		b >>= 1;
	}
}

void InitOpenGL()
{
	glDisable( GL_BLEND );
	glDisable( GL_DEPTH_TEST );
#ifdef GL_POLYGON_SMOOTH
	glDisable( GL_POLYGON_SMOOTH );
#endif // GL_POLYGON_SMOOTH
	glDisable( GL_STENCIL_TEST );
	glEnable( GL_DITHER );

	program = glCreateProgram();
	vertex = glrCreateShader( program, GL_VERTEX_SHADER, OpenGLOutputVertexShader.c_str() );
	fragment = glrCreateShader( program, GL_FRAGMENT_SHADER, OpenGLFragmentShader.c_str() );

	glGenVertexArrays( 1, &vao );
	glBindVertexArray( vao );
	glGenBuffers( 3, &vbo[ 0 ] );

	glrLinkProgram( program );

	glGenTextures( 1, &texture );
}

auto PPU::power(bool reset) -> void {
  //PPUcounter::reset();
  memory::fill<uint16>(output, 1024 * 960);

  if(!reset) {
    for(auto& word : vram) word = 0x0000;
    for(auto& color : cgram) color = 0x0000;
    for(auto& object : objects) object = {};
  }

  latch = {};
  io = {};
  updateVideoMode();

  ItemLimit = 128;
  TileLimit = 128;

  Line::start = 0;
  Line::count = 0;

  frame = {};

	updateVideoPalette();

	InitOpenGL();
}
