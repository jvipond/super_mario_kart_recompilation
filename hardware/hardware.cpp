#include "hardware.hpp"
#include <iostream>
#include "ppu/ppu.hpp"
#ifdef __EMSCRIPTEN__
#include <GLES3/gl32.h>
#else
#include "GL/gl3w.h"
#endif // __EMSCRIPTEN__
#include "../imgui/imgui_impl_sdl.h"
#include "../imgui/imgui_impl_opengl3.h"
#include <SDL.h>

SDSP1 DSP1;

std::tuple<uint32_t, uint32_t> getBankAndOffset( uint32_t addr )
{
	auto bank = ( addr & 0xFF0000 ) >> 16;
	auto bank_offset = ( addr & 0x00FFFF );
	return { bank, bank_offset };
}

extern "C"
{
	union
	{
		uint8_t l;
		uint8_t h;
		uint16_t w = 0;
	} A;
	uint8_t DB = 0;
	uint16_t DP = 0;
	uint8_t PB = 0;
	uint16_t PC = 0;
	uint16_t SP = 0x01ff;
	uint16_t X = 0;
	uint16_t Y = 0;
	uint8_t P = 0;

	bool CF = false;
	bool ZF = false;
	bool IF = false;
	bool DF = false;
	bool XF = false;
	bool MF = false;
	bool VF = false;
	bool NF = false;
	bool EF = true;

	uint8_t ADC8( uint8_t data )
	{
		int result;

		if ( !DF )
		{
			result = A.l + data + CF;
		}
		else
		{
			result = ( A.l & 0x0f ) + ( data & 0x0f ) + ( CF << 0 );
			if ( result > 0x09 ) result += 0x06;
			CF = result > 0x0f;
			result = ( A.l & 0xf0 ) + ( data & 0xf0 ) + ( CF << 4 ) + ( result & 0x0f );
		}

		VF = ~( A.l ^ data ) & ( A.l ^ result ) & 0x80;
		if ( DF && result > 0x9f ) result += 0x60;
		CF = result > 0xff;
		ZF = (uint8_t)result == 0;
		NF = result & 0x80;

		return A.l = result;
	}

	uint16_t ADC16( uint16_t data )
	{
		int result;

		if ( !DF )
		{
			result = A.w + data + CF;
		}
		else
		{
			result = ( A.w & 0x000f ) + ( data & 0x000f ) + ( CF << 0 );
			if ( result > 0x0009 ) result += 0x0006;
			CF = result > 0x000f;
			result = ( A.w & 0x00f0 ) + ( data & 0x00f0 ) + ( CF << 4 ) + ( result & 0x000f );
			if ( result > 0x009f ) result += 0x0060;
			CF = result > 0x00ff;
			result = ( A.w & 0x0f00 ) + ( data & 0x0f00 ) + ( CF << 8 ) + ( result & 0x00ff );
			if ( result > 0x09ff ) result += 0x0600;
			CF = result > 0x0fff;
			result = ( A.w & 0xf000 ) + ( data & 0xf000 ) + ( CF << 12 ) + ( result & 0x0fff );
		}

		VF = ~( A.w ^ data ) & ( A.w ^ result ) & 0x8000;
		if ( DF && result > 0x9fff ) result += 0x6000;
		CF = result > 0xffff;
		ZF = (uint16_t)result == 0;
		NF = result & 0x8000;

		return A.w = result;
	}

	uint8_t SBC8( uint8_t data )
	{
		int result;
		data = ~data;

		if ( !DF )
		{
			result = A.l + data + CF;
		}
		else
		{
			result = ( A.l & 0x0f ) + ( data & 0x0f ) + ( CF << 0 );
			if ( result <= 0x0f ) result -= 0x06;
			CF = result > 0x0f;
			result = ( A.l & 0xf0 ) + ( data & 0xf0 ) + ( CF << 4 ) + ( result & 0x0f );
		}

		VF = ~( A.l ^ data ) & ( A.l ^ result ) & 0x80;
		if ( DF && result <= 0xff ) result -= 0x60;
		CF = result > 0xff;
		ZF = (uint8_t)result == 0;
		NF = result & 0x80;

		return A.l = result;
	}

	uint16_t SBC16( uint16_t data )
	{
		int result;
		data = ~data;

		if ( !DF )
		{
			result = A.w + data + CF;
		}
		else
		{
			result = ( A.w & 0x000f ) + ( data & 0x000f ) + ( CF << 0 );
			if ( result <= 0x000f ) result -= 0x0006;
			CF = result > 0x000f;
			result = ( A.w & 0x00f0 ) + ( data & 0x00f0 ) + ( CF << 4 ) + ( result & 0x000f );
			if ( result <= 0x00ff ) result -= 0x0060;
			CF = result > 0x00ff;
			result = ( A.w & 0x0f00 ) + ( data & 0x0f00 ) + ( CF << 8 ) + ( result & 0x00ff );
			if ( result <= 0x0fff ) result -= 0x0600;
			CF = result > 0x0fff;
			result = ( A.w & 0xf000 ) + ( data & 0xf000 ) + ( CF << 12 ) + ( result & 0x0fff );
		}

		VF = ~( A.w ^ data ) & ( A.w ^ result ) & 0x8000;
		if ( DF && result <= 0xffff ) result -= 0x6000;
		CF = result > 0xffff;
		ZF = (uint16_t)result == 0;
		NF = result & 0x8000;

		return A.w = result;
	}

	void panic( void )
	{
		Hardware::GetInstance().Panic();
	}

	uint8_t read8( const uint32_t address )
	{
		return Hardware::GetInstance().read8( address );
	}

	void write8( const uint32_t address, const uint8_t value )
	{
		Hardware::GetInstance().write8( address, value );
	}

	void doPPUFrame( void )
	{
		Hardware::GetInstance().DoPPUFrame();
	}

	
	void updateInstructionOutput( const uint32_t pc, const char* instructionString )
	{
		Hardware::GetInstance().UpdateInstructionOutput( pc, instructionString );
	}

	void romCycle( const int32_t cycles, const uint32_t implemented )
	{
		Hardware::GetInstance().RomCycle( cycles, implemented );
	}
}

void Hardware::incrementCycleCount( void )
{
	m_SPCTime += 5;
	if ( m_SPCTime > 1024000 / 2 )
	{
		m_SPC.end_frame( 1024000 / 2 );
		m_SPCTime = 0;
	}
}

void Hardware::spcWritePort( const int32_t port, const int32_t data )
{
	incrementCycleCount();
	m_SPC.write_port( m_SPCTime, port, data );
}

int32_t Hardware::spcReadPort( const int32_t port )
{
	incrementCycleCount();
	return m_SPC.read_port( m_SPCTime, port );
}

void Hardware::ProcessAutoJoyPadRead( void )
{
	for ( auto& controller : m_SnesControllers )
	{
		controller.write( 0x4016, 1 );
	}

	for ( auto& controller : m_SnesControllers )
	{
		controller.write( 0x4016, 0 );
	}

	for ( int i = 0; i < 4; i++ ) 
	{
		m_InternalRegisterState.controllerData[ i ] = 0;
	}

	for ( int i = 0; i < 16; i++ )
	{
		uint8_t port1 = m_SnesControllers[0].read( 0x4016 );
		uint8_t port2 = m_SnesControllers[1].read( 0x4017 );

		m_InternalRegisterState.controllerData[ 0 ] <<= 1;
		m_InternalRegisterState.controllerData[ 1 ] <<= 1;
		m_InternalRegisterState.controllerData[ 2 ] <<= 1;
		m_InternalRegisterState.controllerData[ 3 ] <<= 1;

		m_InternalRegisterState.controllerData[ 0 ] |= ( port1 & 0x01 );
		m_InternalRegisterState.controllerData[ 1 ] |= ( port2 & 0x01 );
		m_InternalRegisterState.controllerData[ 2 ] |= ( port1 & 0x02 ) >> 1;
		m_InternalRegisterState.controllerData[ 3 ] |= ( port2 & 0x02 ) >> 1;
	}
}

uint8_t Hardware::dspRead( const uint32_t addr )
{
	return DSP1GetByte( addr );
}

void Hardware::dspWrite( const uint32_t addr, const uint8_t data )
{
	DSP1SetByte( addr, data );
}

void Hardware::PowerOn()
{
	std::cout << "Reached Power On" << std::endl;
	initialiseSDL();

	LoadRom( "Super Mario Kart (USA).sfc" );
	std::cout << "Loaded Rom" << std::endl;
	memset( m_wRam, 0x55, sizeof( m_wRam ) );
	memset( m_sRam, 0xff, sizeof( m_sRam ) );

	m_SnesControllers.push_back( 0 );
	m_SnesControllers.push_back( 1 );
 
	m_SPC.init();
	m_SPC.init_rom( IPL_ROM );
	m_SPC.reset();

	ResetDSP();

	ppufast.power( false );

	std::cout << "Reached Start!" << std::endl;
	//std::exit( EXIT_FAILURE );
	start();

	quit();
}

void Hardware::LoadRom( const char* romPath )
{
	FILE * pFile = fopen( romPath, "rb" );
	if ( pFile )
	{
		auto result = fread( m_rom, 1, sizeof( m_rom ), pFile );
		assert( result == sizeof( m_rom ) );
		if ( result < sizeof( m_rom ) )
		{
			std::exit( EXIT_FAILURE );
		}
	}
	else
	{
		std::cout << "Can't load rom file " << romPath << std::endl;
		std::exit( EXIT_FAILURE );
	}
}

void Hardware::initialiseSDL()
{
	if ( SDL_Init( SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER ) != 0 )
	{
		std::cout << "Error: " << SDL_GetError() << std::endl;
		return;
	}
#ifdef __EMSCRIPTEN__
	const char* glsl_version = "#version 100";
	//const char* glsl_version = "#version 300 es";
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_FLAGS, 0 );
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES );
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_MAJOR_VERSION, 3 );
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_MINOR_VERSION, 0 );
#else
	const char* glsl_version = "#version 130";
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_FLAGS, 0 );
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE );
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_MAJOR_VERSION, 3 );
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_MINOR_VERSION, 0 );
#endif // __EMSCRIPTEN__
	

	SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );
	SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, 24 );
	SDL_GL_SetAttribute( SDL_GL_STENCIL_SIZE, 8 );
	SDL_WindowFlags window_flags = (SDL_WindowFlags)( SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI );

	m_Window = SDL_CreateWindow( "SMK", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1600, 960, window_flags );
	m_GLContext = SDL_GL_CreateContext( m_Window );
	SDL_GL_SetSwapInterval( 1 ); // Enable vsync

	if ( m_GLContext == nullptr )
	{
		std::cout << "Failed to initialize GL context!" << std::endl;
		std::exit( EXIT_FAILURE );
	}

#ifndef __EMSCRIPTEN__
	int err = gl3wInit();
	if ( err != GL3W_OK )
	{
		std::cout << "Failed to initialize OpenGL loader!\n" << std::endl;
		return;
	}
#endif // __EMSCRIPTEN__

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGui::StyleColorsDark();

	ImGui_ImplSDL2_InitForOpenGL( m_Window, m_GLContext );
	ImGui_ImplOpenGL3_Init( glsl_version );
	SDL_GL_MakeCurrent( m_Window, m_GLContext );
}

void Hardware::quit()
{
	// Cleanup
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplSDL2_Shutdown();
	ImGui::DestroyContext();

	SDL_GL_DeleteContext( m_GLContext );
	SDL_DestroyWindow( m_Window );
	SDL_Quit();

	std::exit( EXIT_SUCCESS );
}

Hardware& Hardware::GetInstance()
{
	static Hardware instance;
	return instance;
}

uint8_t Hardware::read8( const uint32_t address )
{
	auto[ bank, bank_offset ] = getBankAndOffset( address );

	if ( bank <= 0x3f && bank_offset <= 0x1fff )
	{
		return m_wRam[ 0x1fff & address ];
	}
	else if ( ( bank <= 0x3f || ( bank >= 0x80 && bank <= 0xbf ) ) && bank_offset >= 0x2140 && bank_offset <= 0x217F )
	{
		return spcReadPort( bank_offset & 0x3 );
	}
	else if ( ( bank <= 0x3f || ( bank >= 0x80 && bank <= 0xbf ) ) && bank_offset == 0x2180 )
	{
		uint8_t value = m_wRam[ m_wRamPosition ];
		m_wRamPosition = ( m_wRamPosition + 1 ) & 0x1FFFF;
		return value;
	}
	else if ( ( bank <= 0x3f || ( bank >= 0x80 && bank <= 0xbf ) ) && bank_offset == 0x4212 )
	{
		// only thing that checks this register is inside irq to make sure it blocks until the next hblank. So due to way system is implemented
		// we just return 0x40 indicating that hblank is set.
		return 0x40;
	}
	else if ( ( bank <= 0x3f || ( bank >= 0x80 && bank <= 0xbf ) ) && (bank_offset == 0x4016 || bank_offset ==0x4017)/*bank_offset >= 0x4016 && bank_offset <= 0x4017*/ )
	{
		uint8_t value = bank_offset == 0x4016 ? m_SnesControllers[0].read( 0x4016 ) : m_SnesControllers[ 1 ].read( 0x4017 );
		return value;
	}
	else if ( ( bank <= 0x3f || ( bank >= 0x80 && bank <= 0xbf ) ) && bank_offset >= 0x4214 && bank_offset <= 0x421F )
	{
		switch ( bank_offset )
		{
		case 0x4214: return m_aluMulDivState.rddiv >> 0;  //RDDIVL
		case 0x4215: return m_aluMulDivState.rddiv >> 8;  //RDDIVH
		case 0x4216: return m_aluMulDivState.rdmpy >> 0;  //RDMPYL
		case 0x4217: return m_aluMulDivState.rdmpy >> 8;  //RDMPYH

		case 0x4218: return (uint8_t)m_InternalRegisterState.controllerData[ 0 ];
		case 0x4219: return (uint8_t)( m_InternalRegisterState.controllerData[ 0 ] >> 8 );
		case 0x421A: return (uint8_t)m_InternalRegisterState.controllerData[ 1 ];
		case 0x421B: return (uint8_t)( m_InternalRegisterState.controllerData[ 1 ] >> 8 );
		case 0x421C: return (uint8_t)m_InternalRegisterState.controllerData[ 2 ];
		case 0x421D: return (uint8_t)( m_InternalRegisterState.controllerData[ 2 ] >> 8 );
		case 0x421E: return (uint8_t)m_InternalRegisterState.controllerData[ 3 ];
		case 0x421F: return (uint8_t)( m_InternalRegisterState.controllerData[ 3 ] >> 8 );
		default: return 0;
		}
	}
	else if ( ( bank <= 0x3f || ( bank >= 0x80 && bank <= 0xbf ) ) && bank_offset >= 0x2104 && bank_offset <= 0x213f )
	{
		uint8_t data = 0;
		return ppufast.readIO( address, data );
	}
	else if ( ( bank <= 0x1f || ( bank >= 0x80 && bank <= 0x9f ) ) && ( bank_offset >= 0x6000 && bank_offset <= 0x7001 ) )
	{
		return dspRead( address & 0xffff );
	}
	else if ( ( ( bank >= 0x20 && bank <= 0x3f ) || ( bank >= 0xa0 && bank <= 0xbf ) ) && ( bank_offset >= 0x6000 && bank_offset <= 0x7fff ) )
	{
		const uint32_t offset = ( address & 0x7ff );
		return m_sRam[ offset ];
	}
	else if ( ( bank <= 0x3f || ( bank >= 0x80 && bank <= 0xbf ) ) && bank_offset >= 0x4300 && bank_offset <= 0x437A )
	{
		return m_dmaController.Read( bank_offset );
	}
	else if ( bank <= 0x1f && bank_offset >= 0x8000 && bank_offset <= 0xffff )
	{
		return m_rom[ address & 0x7ffff ];
	}
	else if ( bank >= 0x20 && bank <= 0x3f && bank_offset <= 0x1fff )
	{
		return m_wRam[ 0x1fff & address ];
	}
	else if ( bank >= 0x20 && bank <= 0x3f && bank_offset >= 0x8000 && bank_offset <= 0xffff )
	{
		return m_rom[ address - 0x200000 ];
	}
	else if ( bank >= 0x40 && bank <= 0x7d && bank_offset <= 0xffff )
	{
		return m_rom[ address - 0x400000 ];
	}
	else if ( bank >= 0x7e && bank <= 0x7f && bank_offset <= 0xffff )
	{
		return m_wRam[ address - 0x7e0000 ];
	}
	else if ( bank >= 0xc0 && bank <= 0xfd && bank_offset <= 0xffff )
	{
		return m_rom[ address - 0xc00000 ];
	}
	else if ( bank >= 0xfe && bank <= 0xff && bank_offset <= 0xffff )
	{
		return m_rom[ address - 0xfe0000 ];
	}
	else if ( bank >= 0x80 && bank <= 0x9f && bank_offset <= 0x1fff )
	{
		return m_wRam[ 0x1fff & address ];
	}
	else if ( bank >= 0x80 && bank <= 0x9f && bank_offset >= 0x8000 && bank_offset <= 0xffff )
	{
		return m_rom[ address - 0x800000 ];
	}
	else if ( bank >= 0xa0 && bank <= 0xbf && bank_offset <= 0x1fff )
	{
		return m_wRam[ 0x1fff & address ];
	}

	return 0;
}

void Hardware::write8( const uint32_t address, const uint8_t value )
{
	auto[ bank, bank_offset ] = getBankAndOffset( address );

	if ( bank <= 0x3f && bank_offset <= 0x1fff )
	{
		m_wRam[ 0x1fff & address ] = value;
	}
	else if ( ( bank <= 0x3f || ( bank >= 0x80 && bank <= 0xbf ) ) && bank_offset >= 0x2140 && bank_offset <= 0x217F )
	{
		spcWritePort( bank_offset & 0x3, value );
	}
	else if ( ( bank <= 0x3f || ( bank >= 0x80 && bank <= 0xbf ) ) && ( bank_offset >= 0x2180 && bank_offset <= 0x2183 ) )
	{
		switch ( address & 0xFFFF ) {
		case 0x2180:
			m_wRam[ m_wRamPosition ] = value;
			m_wRamPosition = ( m_wRamPosition + 1 ) & 0x1FFFF;
			break;

		case 0x2181: m_wRamPosition = ( m_wRamPosition & 0x1FF00 ) | value; break;
		case 0x2182: m_wRamPosition = ( m_wRamPosition & 0x100FF ) | ( value << 8 ); break;
		case 0x2183: m_wRamPosition = ( m_wRamPosition & 0xFFFF ) | ( ( value & 0x01 ) << 16 ); break;
		}
	}
	else if ( ( bank <= 0x3f || ( bank >= 0x80 && bank <= 0xbf ) ) && bank_offset >= 0x2100 && bank_offset <= 0x2133 )
	{
		ppufast.writeIO( address, value );
	}
	else if ( ( bank <= 0x3f || ( bank >= 0x80 && bank <= 0xbf ) ) && ( bank_offset >= 0x4202 && bank_offset <= 0x4206 ) )
	{
		switch ( bank_offset )
		{
		case 0x4202:  //WRMPYA
			m_aluMulDivState.wrmpya = value;
			return;

		case 0x4203:  //WRMPYB
			m_aluMulDivState.rdmpy = 0;

			m_aluMulDivState.wrmpyb = value;
			m_aluMulDivState.rddiv = m_aluMulDivState.wrmpyb << 8 | m_aluMulDivState.wrmpya;

			m_aluMulDivState.rdmpy = m_aluMulDivState.wrmpya * m_aluMulDivState.wrmpyb;
			return;

		case 0x4204:  //WRDIVL
			m_aluMulDivState.wrdiva = m_aluMulDivState.wrdiva & 0xff00 | value << 0;
			return;

		case 0x4205:  //WRDIVH
			m_aluMulDivState.wrdiva = m_aluMulDivState.wrdiva & 0x00ff | value << 8;
			return;

		case 0x4206:  //WRDIVB
			m_aluMulDivState.rdmpy = m_aluMulDivState.wrdiva;
			m_aluMulDivState.wrdivb = value;

			if ( m_aluMulDivState.wrdivb )
			{
				m_aluMulDivState.rddiv = m_aluMulDivState.wrdiva / m_aluMulDivState.wrdivb;
				m_aluMulDivState.rdmpy = m_aluMulDivState.wrdiva % m_aluMulDivState.wrdivb;
			}
			else
			{
				m_aluMulDivState.rddiv = 0xffff;
				m_aluMulDivState.rdmpy = m_aluMulDivState.wrdiva;
			}
			return;
		default:
			return;
		}
	}
	else if ( ( bank <= 0x3f || ( bank >= 0x80 && bank <= 0xbf ) ) && ( bank_offset == 0x4016 /*|| bank_offset == 0x4017*/ )/*bank_offset >= 0x4016 && bank_offset <= 0x4017*/ )
	{
		for ( auto& controller : m_SnesControllers )
		{
			controller.write( bank_offset, value );
		}
	}
	else if ( ( bank <= 0x3f || ( bank >= 0x80 && bank <= 0xbf ) ) && bank_offset >= 0x4200 && bank_offset <= 0x420A )
	{
		switch ( bank_offset )
		{
		case 0x4200:
			m_InternalRegisterState.enableNmi = ( value & 0x80 ) != 0;
			m_InternalRegisterState.enableVerticalIrq = ( value & 0x20 ) != 0;
			m_InternalRegisterState.enableHorizontalIrq = ( value & 0x10 ) != 0;
			m_InternalRegisterState.enableAutoJoypadRead = ( value & 0x01 ) != 0;
			break;
		case 0x4201:
			m_InternalRegisterState.ioPortOutput = value;
			break;
		case 0x4207: m_InternalRegisterState.horizontalTimer = ( m_InternalRegisterState.horizontalTimer & 0x100 ) | value; break;
		case 0x4208: m_InternalRegisterState.horizontalTimer = ( m_InternalRegisterState.horizontalTimer & 0xFF ) | ( ( value & 0x01 ) << 8 ); break;

		case 0x4209: m_InternalRegisterState.verticalTimer = ( m_InternalRegisterState.verticalTimer & 0x100 ) | value; break;
		case 0x420A: m_InternalRegisterState.verticalTimer = ( m_InternalRegisterState.verticalTimer & 0xFF ) | ( ( value & 0x01 ) << 8 ); break;
		}
	}
	else if ( ( bank <= 0x3f || ( bank >= 0x80 && bank <= 0xbf ) ) && ( bank_offset == 0x420B || bank_offset == 0x420C || ( bank_offset >= 0x4300 && bank_offset <= 0x437A ) ) )
	{
		m_dmaController.Write( bank_offset, value );
	}
	else if ( ( bank <= 0x1f || ( bank >= 0x80 && bank <= 0x9f ) ) && ( bank_offset >= 0x6000 && bank_offset <= 0x7001 ) )
	{
		dspWrite( address & 0xffff, value );
	}
	else if ( ( ( bank >= 0x20 && bank <= 0x3f ) || ( bank >= 0xa0 && bank <= 0xbf ) ) && ( bank_offset >= 0x6000 && bank_offset <= 0x7fff ) )
	{
		const uint32_t offset = ( address & 0x7ff );
		m_sRam[ offset ] = value;
	}
	else if ( bank >= 0x20 && bank <= 0x3f && bank_offset <= 0x1fff )
	{
		m_wRam[ 0x1fff & address ] = value;
	}
	else if ( bank >= 0x7e && bank <= 0x7f && bank_offset <= 0xffff )
	{
		m_wRam[ address - 0x7e0000 ] = value;
	}
	else if ( bank >= 0x80 && bank <= 0x9f && bank_offset <= 0x1fff )
	{
		m_wRam[ 0x1fff & address ] = value;
	}
	else if ( bank >= 0xa0 && bank <= 0xbf && bank_offset <= 0x1fff )
	{
		m_wRam[ 0x1fff & address ] = value;
	}
}

void Hardware::DoPPUFrame()
{
	if ( m_RenderSnesOutputToScreen )
	{
		ppufast.doFrame( m_dmaController, m_InternalRegisterState, m_RenderSnesOutputToScreen ? m_Window : nullptr );
	}
	if ( m_InternalRegisterState.enableAutoJoypadRead )
	{
		ProcessAutoJoyPadRead();
	}
}

void Hardware::UpdateInstructionOutput( const uint32_t pc, const char* instructionString )
{
	RegisterState rs = { A.w, DB, DP, PB, PC, SP, X, Y, CF, ZF, IF, DF, XF, MF, VF, NF, EF };

	if ( m_InstructionTrace.size() >= 128 )
	{
		m_InstructionTrace.pop_front();
	}
	m_InstructionTrace.push_back( { pc, instructionString, rs, 1 } );
}

void Hardware::Panic( void )
{
	m_AutoStep = false;
	m_DoRender = true;
	m_RenderSnesOutputToScreen = false;
	romCycle( 0, false );
	std::cout << "Exited with error" << std::endl;
	std::exit( EXIT_FAILURE );
}

void Hardware::RomCycle( const int32_t cycles, const uint32_t implemented )
{
	if ( m_RenderSnesOutputToScreen )
	{
		incrementCycleCount();
		SDL_Event event;
		while ( SDL_PollEvent( &event ) )
		{
			ImGui_ImplSDL2_ProcessEvent( &event );
			if ( event.type == SDL_QUIT )
				quit();
			if ( event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID( m_Window ) )
				quit();
		}

		for ( auto& controller : m_SnesControllers )
		{
			controller.UpdateKeyboardState();
		}
		/*m_SnesController.UpdateKeyboardState();*/

		/*if ( m_InternalRegisterState.enableAutoJoypadRead )
		{
			ProcessAutoJoyPadRead();
		}*/
	}
	else
	{
		if ( !implemented )
		{
			m_AutoStep = false;
			m_DoRender = true;
		}
		incrementCycleCount();
		std::get<3>( m_InstructionTrace.back() ) = implemented;
		ImGuiIO& io = ImGui::GetIO();

		ImVec4 clear_color = ImVec4( 0.45f, 0.55f, 0.60f, 1.00f );

		bool scrollToBottom = true;
		bool done = false;
		while ( m_DoRender && !done )
		{
			SDL_Event event;
			while ( SDL_PollEvent( &event ) )
			{
				ImGui_ImplSDL2_ProcessEvent( &event );
				if ( event.type == SDL_QUIT )
					quit();
				if ( event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID( m_Window ) )
					quit();
			}

			// Start the Dear ImGui frame
			ImGui_ImplOpenGL3_NewFrame();
			ImGui_ImplSDL2_NewFrame( m_Window );
			ImGui::NewFrame();
			{
				ImGui::Begin( "Register Status" );
				ImGui::Text( "A = 0x%04hX", A.w );
				ImGui::Text( "X = 0x%04hX", X );
				ImGui::Text( "Y = 0x%04hX", Y );
				ImGui::Text( "DB = 0x%02hhX", DB );
				ImGui::Text( "DP = 0x%04hX", DP );
				ImGui::Text( "PB = 0x%02hhX", PB );
				ImGui::Text( "PC = 0x%04hX", PC );
				ImGui::Text( "SP = 0x%04hX", SP );

				ImGui::Text( "P = %c%c%c%c%c%c%c%c", NF ? 'N' : 'n', VF ? 'V' : 'v', MF ? 'M' : 'm',
					XF ? 'X' : 'x', DF ? 'D' : 'd', IF ? 'I' : 'i',
					ZF ? 'Z' : 'z', CF ? 'C' : 'c' );

				done = m_AutoStep;
				if ( ImGui::Button( "Single Step" ) )
				{
					done = true;
				}

				if ( ImGui::Button( "Auto Step" ) )
				{
					m_AutoStep = !m_AutoStep;
					done = m_AutoStep;
				}

				if ( ImGui::Button( "Continue" ) )
				{
					m_AutoStep = true;
					done = true;
					m_DoRender = false;
					m_RenderSnesOutputToScreen = true;
				}

				ImGui::End();
			}

			{
				ImGui::Begin( "wRam" );
				m_MemoryEditor.DrawContents( m_wRam, sizeof( m_wRam ), static_cast<size_t>( 0x7E0000 ) );
				ImGui::End();
			}

			{
				ImGui::Begin( "rom" );
				m_MemoryEditor.DrawContents( m_rom, sizeof( m_rom ) );
				ImGui::End();
			}

			{
				ImGui::Begin( "Instruction Trace" );
				ImGui::Columns( 11, "Instruction Trace columns" );
				ImGui::Separator();
				ImGui::SetColumnWidth( -1, 60.0f );
				ImGui::Text( "A" ); ImGui::NextColumn();
				ImGui::SetColumnWidth( -1, 60.0f );
				ImGui::Text( "X" ); ImGui::NextColumn();
				ImGui::SetColumnWidth( -1, 60.0f );
				ImGui::Text( "Y" ); ImGui::NextColumn();
				ImGui::SetColumnWidth( -1, 50.0f );
				ImGui::Text( "DB" ); ImGui::NextColumn();
				ImGui::SetColumnWidth( -1, 60.0f );
				ImGui::Text( "DP" ); ImGui::NextColumn();
				ImGui::SetColumnWidth( -1, 50.0f );
				ImGui::Text( "PB" ); ImGui::NextColumn();
				ImGui::SetColumnWidth( -1, 60.0f );
				ImGui::Text( "SP" ); ImGui::NextColumn();
				ImGui::SetColumnWidth( -1, 75.0f );
				ImGui::Text( "P" ); ImGui::NextColumn();
				ImGui::SetColumnWidth( -1, 75.0f );
				ImGui::Text( "PC" ); ImGui::NextColumn();
				ImGui::SetColumnWidth( -1, 350.0f );
				ImGui::Text( "Instruction" ); ImGui::NextColumn();
				ImGui::SetColumnWidth( -1, 150.0f );
				ImGui::Text( "Implemented" ); ImGui::NextColumn();
				ImGui::Separator();
				for ( auto&[ pc, instructionString, rs, hasBeenImplemented ] : m_InstructionTrace )
				{
					ImGui::TextColored( ImVec4( 0.8f, 0.8f, 0.8f, 1.0f ), "0x%04hX", rs.A ); ImGui::NextColumn();
					ImGui::TextColored( ImVec4( 0.8f, 0.8f, 0.8f, 1.0f ), "0x%04hX", rs.X ); ImGui::NextColumn();
					ImGui::TextColored( ImVec4( 0.8f, 0.8f, 0.8f, 1.0f ), "0x%04hX", rs.Y ); ImGui::NextColumn();
					ImGui::TextColored( ImVec4( 0.8f, 0.8f, 0.8f, 1.0f ), "0x%02hhX", rs.DB ); ImGui::NextColumn();
					ImGui::TextColored( ImVec4( 0.8f, 0.8f, 0.8f, 1.0f ), "0x%04hX", rs.DP ); ImGui::NextColumn();
					ImGui::TextColored( ImVec4( 0.8f, 0.8f, 0.8f, 1.0f ), "0x%02hhX", rs.PB ); ImGui::NextColumn();
					ImGui::TextColored( ImVec4( 0.8f, 0.8f, 0.8f, 1.0f ), "0x%04hX", rs.SP ); ImGui::NextColumn();
					ImGui::TextColored( ImVec4( 0.8f, 0.8f, 0.8f, 1.0f ), "%c%c%c%c%c%c%c%c", rs.NF ? 'N' : 'n', rs.VF ? 'V' : 'v', rs.MF ? 'M' : 'm',
						rs.XF ? 'X' : 'x', rs.DF ? 'D' : 'd', rs.IF ? 'I' : 'i',
						rs.ZF ? 'Z' : 'z', rs.CF ? 'C' : 'c' ); ImGui::NextColumn();
					ImGui::TextColored( ImVec4( 1.0f, 1.0f, 0.0f, 1.0f ), "$%06X", pc ); ImGui::NextColumn();
					ImGui::TextColored( ImVec4( 1.0f, 1.0f, 0.0f, 1.0f ), "%s", instructionString ); ImGui::NextColumn();
					ImGui::TextColored( ImVec4( 1.0f, 0.0f, 1.0f, 1.0f ), "%s", hasBeenImplemented == 0 ? "[NOT IMPLEMENTED]" : "" ); ImGui::NextColumn();
					ImGui::Separator();
				}
				ImGui::Columns( 1 );
				if ( scrollToBottom )
				{
					ImGui::SetScrollHere( 1.0f );
				}
				scrollToBottom = false;
				ImGui::End();
			}

			// Rendering
			ImGui::Render();
			glViewport( 0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y );
			glClearColor( clear_color.x, clear_color.y, clear_color.z, clear_color.w );
			glClear( GL_COLOR_BUFFER_BIT );
			ImGui_ImplOpenGL3_RenderDrawData( ImGui::GetDrawData() );
			SDL_GL_SwapWindow( m_Window );
		}
	}
}

Hardware::SnesController::SnesController( const uint8_t port )
: m_Strobe( false )
, m_Port( port )
, m_StateBuffer( 0 )
, m_KeyboardState( nullptr )
{
	
}

bool Hardware::SnesController::IsCurrentPort( const uint32_t address )
{
	return m_Port == ( address - 0x4016 );
}

void Hardware::SnesController::RefreshStateBuffer()
{
	if ( m_Port == 0 )
	{
		m_StateBuffer = 0;
		m_StateBuffer = m_KeyboardState[ SDL_SCANCODE_Z ] |
			( m_KeyboardState[ SDL_SCANCODE_X ] << 1 ) |
			( m_KeyboardState[ SDL_SCANCODE_D ] << 2 ) |
			( m_KeyboardState[ SDL_SCANCODE_F ] << 3 ) |
			( m_KeyboardState[ SDL_SCANCODE_UP ] << 4 ) |
			( m_KeyboardState[ SDL_SCANCODE_DOWN ] << 5 ) |
			( m_KeyboardState[ SDL_SCANCODE_LEFT ] << 6 ) |
			( m_KeyboardState[ SDL_SCANCODE_RIGHT ] << 7 ) |
			( m_KeyboardState[ SDL_SCANCODE_A ] << 8 ) |
			( m_KeyboardState[ SDL_SCANCODE_S ] << 9 ) |
			( m_KeyboardState[ SDL_SCANCODE_J ] << 10 ) |
			( m_KeyboardState[ SDL_SCANCODE_K ] << 11 );
	}
	else if ( m_Port == 1 )
	{
		m_StateBuffer = 0;
		m_StateBuffer = m_KeyboardState[ SDL_SCANCODE_Y ] |
			( m_KeyboardState[ SDL_SCANCODE_U ] << 1 ) |
			( m_KeyboardState[ SDL_SCANCODE_I ] << 2 ) |
			( m_KeyboardState[ SDL_SCANCODE_O ] << 3 ) |
			( m_KeyboardState[ SDL_SCANCODE_G ] << 4 ) |
			( m_KeyboardState[ SDL_SCANCODE_H ] << 5 ) |
			( m_KeyboardState[ SDL_SCANCODE_R ] << 6 ) |
			( m_KeyboardState[ SDL_SCANCODE_T ] << 7 ) |
			( m_KeyboardState[ SDL_SCANCODE_V ] << 8 ) |
			( m_KeyboardState[ SDL_SCANCODE_B ] << 9 ) |
			( m_KeyboardState[ SDL_SCANCODE_N ] << 10 ) |
			( m_KeyboardState[ SDL_SCANCODE_M ] << 11 );
	}
}

uint8_t Hardware::SnesController::read( const uint32_t address )
{
	uint8_t output = 0;

	if ( IsCurrentPort( address ) ) 
	{
		if ( m_Strobe ) 
		{
			RefreshStateBuffer();
		}

		if ( m_Port >= 2 )
		{
			output = ( m_StateBuffer & 0x01 ) << 1;  //P3/P4 are reported in bit 2
		}
		else
		{
			output = m_StateBuffer & 0x01;
		}
		m_StateBuffer >>= 1;

		//"All subsequent reads will return D=1 on an authentic controller but may return D=0 on third party controllers."
		m_StateBuffer |= 0x8000;
	}

	uint8_t value = 0;//address == 0x4016 ? 0xFC : 0xE0;
	return value | output;
}

void Hardware::SnesController::write( const uint32_t address, const uint8_t data )
{
	bool prevStrobe = m_Strobe;
	m_Strobe = ( data & 0x01 ) == 0x01;

	if ( prevStrobe && !m_Strobe )
	{
		RefreshStateBuffer();
	}
}

void Hardware::SnesController::UpdateKeyboardState()
{
	m_KeyboardState = SDL_GetKeyboardState( nullptr );
}