#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <cassert>
#include "GL/gl3w.h"
#include <SDL.h>
#include <tuple>
#include "imgui/imgui.h"
#include "imgui/imgui_impl_sdl.h"
#include "imgui/imgui_impl_opengl3.h"
#include "imgui/imgui_memory_editor.h"
#include <vector>
#include <deque>
#include <string>
#include <fstream>
#include "spc/SNES_SPC.h"
#include "dsp/dsp.h"
#include "Utils.hpp"

static constexpr bool compareToDebugLog = false;

extern "C"
{
	int16_t A = 0;
	int8_t DB = 0;
	int16_t DP = 0;
	int8_t PB = 0;
	int16_t PC = 0;
	int16_t SP = 0;
	int16_t X = 0;
	int16_t Y = 0;
	int8_t P = 0;

	int8_t wRam[ 0x20000 ] = { 0 };
	int8_t rom[ 0x80000 ] = { 0 };

	void start( void );
	SDL_Window* window = nullptr;
	SDL_GLContext gl_context = nullptr;

	void quit( void )
	{
		// Cleanup
		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplSDL2_Shutdown();
		ImGui::DestroyContext();

		SDL_GL_DeleteContext( gl_context );
		SDL_DestroyWindow( window );
		SDL_Quit();

		std::exit( EXIT_SUCCESS );
	}

	struct RegisterState
	{
		int16_t A = 0;
		int8_t DB = 0;
		int16_t DP = 0;
		int8_t PB = 0;
		int16_t PC = 0;
		int16_t SP = 0;
		int16_t X = 0;
		int16_t Y = 0;
		int8_t P = 0;
	};

	struct Snes9xLogState
	{
		std::string Text;
		int16_t A = 0;
		int16_t X = 0;
		int16_t Y = 0;
		int16_t DP = 0;
		int8_t DB = 0;
		int16_t SP = 0;
		std::string P;
	};
	
	std::vector<Snes9xLogState> logState;
	uint32_t currentLogStateIndex = 0;
	static bool autoStep = true;
	static bool render = false;

	std::deque<std::tuple<uint32_t, const char*, RegisterState, uint32_t>> instructionTrace;
	void updateInstructionOutput( const uint32_t pc, const char* instructionString )
	{
		RegisterState rs = { A, DB, DP, PB, PC, SP, X, Y, P };
		
		if ( instructionTrace.size() >= 128 )
		{
			instructionTrace.pop_front();
		}
		instructionTrace.push_back( { pc, instructionString, rs, 1 } );

		if ( pc == 0x80805A )
		{
			autoStep = false;
			render = true;
		}

		if constexpr ( compareToDebugLog )
		{
			if ( currentLogStateIndex < logState.size() )
			{
				const Snes9xLogState& ls = logState[ currentLogStateIndex ];
				char processorRegisterText[256];
				std::sprintf( processorRegisterText, "P = e%c%c%c%c%c%c%c%c", ( P & ( 1 << 7 ) ) ? 'N' : 'n', ( P & ( 1 << 6 ) ) ? 'V' : 'v', ( P & ( 1 << 5 ) ) ? 'M' : 'm',
					( P & ( 1 << 4 ) ) ? 'X' : 'x', ( P & ( 1 << 3 ) ) ? 'D' : 'd', ( P & ( 1 << 2 ) ) ? 'I' : 'i',
					( P & ( 1 << 1 ) ) ? 'Z' : 'z', ( P & ( 1 << 0 ) ) ? 'C' : 'c' );

				auto p = std::string( processorRegisterText );
				if ( !( rs.A == ls.A && rs.X == ls.X && rs.Y == ls.Y && rs.DP == ls.DP && rs.DB == ls.DB && rs.SP == ls.SP && p == ls.P ) )
				{
					autoStep = false;
					render = true;
				}
				assert( rs.A == ls.A && rs.X == ls.X && rs.Y == ls.Y && rs.DP == ls.DP && rs.DB == ls.DB && rs.SP == ls.SP );
				
				std::string is( instructionString );
				auto pos = is.find_first_of( " " );
				if ( pos != std::string::npos )
				{
					is = is.substr( 0, pos );
				}
				if ( ls.Text.find( is ) != std::string::npos )
				{
					currentLogStateIndex++;
				}
			}
		}
	}

	static MemoryEditor mem_edit;
	void romCycle( const int32_t cycles, const uint32_t implemented )
	{
		std::get<3>( instructionTrace.back() ) = implemented;
		ImGuiIO& io = ImGui::GetIO();

		ImVec4 clear_color = ImVec4( 0.45f, 0.55f, 0.60f, 1.00f );

		bool scrollToBottom = true;
		bool done = false;
		while ( render && !done )
		{
			SDL_Event event;
			while ( SDL_PollEvent( &event ) )
			{
				ImGui_ImplSDL2_ProcessEvent( &event );
				if ( event.type == SDL_QUIT )
					quit();
				if ( event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID( window ) )
					quit();
			}

			// Start the Dear ImGui frame
			ImGui_ImplOpenGL3_NewFrame();
			ImGui_ImplSDL2_NewFrame( window );
			ImGui::NewFrame();
			{
				ImGui::Begin( "Register Status" );
				ImGui::Text( "A = 0x%04hX", A );
				ImGui::Text( "X = 0x%04hX", X );
				ImGui::Text( "Y = 0x%04hX", Y );
				ImGui::Text( "DB = 0x%02hhX", DB );
				ImGui::Text( "DP = 0x%04hX", DP );
				ImGui::Text( "PB = 0x%02hhX", PB );
				ImGui::Text( "PC = 0x%04hX", PC );
				ImGui::Text( "SP = 0x%04hX", SP );

				ImGui::Text( "P = %c%c%c%c%c%c%c%c", (P & (1 << 7)) ? 'N' : 'n', ( P & ( 1 << 6 ) ) ? 'V' : 'v', ( P & ( 1 << 5 ) ) ? 'M' : 'm',
																						 ( P & ( 1 << 4 ) ) ? 'X' : 'x', ( P & ( 1 << 3 ) ) ? 'D' : 'd', ( P & ( 1 << 2 ) ) ? 'I' : 'i',
																						 ( P & ( 1 << 1 ) ) ? 'Z' : 'z', ( P & ( 1 << 0 ) ) ? 'C' : 'c' );

				done = autoStep;
				if ( ImGui::Button( "Single Step" ) )
				{
					done = true;
				}

				if ( ImGui::Button( "Auto Step" ) )
				{
					autoStep = !autoStep;
					done = autoStep;
				}

				ImGui::End();
			}

			{
				ImGui::Begin( "wRam" );
				mem_edit.DrawContents( wRam, sizeof( wRam ), static_cast<size_t>( 0x7E0000 ) );
				ImGui::End();
			}

			{
				ImGui::Begin( "rom" );
				mem_edit.DrawContents( rom, sizeof( rom ) );
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
				for ( auto& [ pc, instructionString, rs, hasBeenImplemented ] : instructionTrace )
				{
					ImGui::TextColored( ImVec4( 0.8f, 0.8f, 0.8f, 1.0f ), "0x%04hX", rs.A ); ImGui::NextColumn();
					ImGui::TextColored( ImVec4( 0.8f, 0.8f, 0.8f, 1.0f ), "0x%04hX", rs.X ); ImGui::NextColumn();
					ImGui::TextColored( ImVec4( 0.8f, 0.8f, 0.8f, 1.0f ), "0x%04hX", rs.Y ); ImGui::NextColumn();
					ImGui::TextColored( ImVec4( 0.8f, 0.8f, 0.8f, 1.0f ), "0x%02hhX", rs.DB ); ImGui::NextColumn();
					ImGui::TextColored( ImVec4( 0.8f, 0.8f, 0.8f, 1.0f ), "0x%04hX", rs.DP ); ImGui::NextColumn();
					ImGui::TextColored( ImVec4( 0.8f, 0.8f, 0.8f, 1.0f ), "0x%02hhX", rs.PB ); ImGui::NextColumn();
					ImGui::TextColored( ImVec4( 0.8f, 0.8f, 0.8f, 1.0f ), "0x%04hX", rs.SP ); ImGui::NextColumn();
					ImGui::TextColored( ImVec4( 0.8f, 0.8f, 0.8f, 1.0f ), "%c%c%c%c%c%c%c%c", ( rs.P & ( 1 << 7 ) ) ? 'N' : 'n', ( rs.P & ( 1 << 6 ) ) ? 'V' : 'v', ( rs.P & ( 1 << 5 ) ) ? 'M' : 'm',
						( rs.P & ( 1 << 4 ) ) ? 'X' : 'x', ( rs.P & ( 1 << 3 ) ) ? 'D' : 'd', ( rs.P & ( 1 << 2 ) ) ? 'I' : 'i',
						( rs.P & ( 1 << 1 ) ) ? 'Z' : 'z', ( rs.P & ( 1 << 0 ) ) ? 'C' : 'c' ); ImGui::NextColumn();
					ImGui::TextColored( ImVec4( 1.0f, 1.0f, 0.0f, 1.0f ), "$%06X", pc ); ImGui::NextColumn();
					ImGui::TextColored( ImVec4( 1.0f, 1.0f, 0.0f, 1.0f ), instructionString ); ImGui::NextColumn();
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
			SDL_GL_SwapWindow( window );
		}
	}

	void panic( void )
	{
		std::cout << "Exited with error" << std::endl;
		std::exit( EXIT_FAILURE );
	}

	uint8_t iplRom[] = { 0xCD, 0xEF, 0xBD, 0xE8, 0x00, 0xC6, 0x1D, 0xD0, 0xFC, 0x8F, 0xAA, 0xF4, 0x8F, 0xBB, 0xF5, 0x78,
											 0xCC, 0xF4, 0xD0, 0xFB, 0x2F, 0x19, 0xEB, 0xF4, 0xD0, 0xFC, 0x7E, 0xF4, 0xD0, 0x0B, 0xE4, 0xF5,
											 0xCB, 0xF4, 0xD7, 0x00, 0xFC, 0xD0, 0xF3, 0xAB, 0x01, 0x10, 0xEF, 0x7E, 0xF4, 0x10, 0xEB, 0xBA,
											 0xF6, 0xDA, 0x00, 0xBA, 0xF4, 0xC4, 0xF4, 0xDD, 0x5D, 0xD0, 0xDB, 0x1F, 0x00, 0x00, 0xC0, 0xFF };

	SNES_SPC snesSPC;
	static int32_t stime = 0;

	void incrementCycleCount( void )
	{
		stime += 1;
		if ( stime > 1024000 / 2 )
		{
			snesSPC.end_frame( 1024000 / 2 );
			stime = 0;
		}
	}

	void spcWritePort( int32_t port, int32_t data )
	{
		incrementCycleCount();
		snesSPC.write_port( stime, port, data );
	}

	int32_t spcReadPort( int32_t port )
	{
		incrementCycleCount();
		return snesSPC.read_port( stime, port );
	}

	SDSP1 DSP1;
	
	uint8_t dspRead( uint32_t addr )
	{
		return DSP1GetByte( addr );
	}

	void dspWrite( uint32_t addr, uint8_t data )
	{
		DSP1SetByte( addr, data );
	}

	uint8_t load8( const uint32_t address )
	{
		auto[ bank, bank_offset ] = getBankAndOffset( address );

		if ( bank <= 0x1f && bank_offset <= 0x1fff )
		{
			return wRam[ 0x1fff & address ];
		}
		else if ( ( bank <= 0x3f || ( bank >= 0x80 && bank <= 0xbf ) ) && bank_offset >= 0x2140 && bank_offset <= 0x217F )
		{
			return spcReadPort( bank_offset & 0x3 );
		}
		else if ( bank <= 0x3f && ( bank_offset >= 0x6000 && bank_offset <= 0x7001 ) )
		{
			return dspRead( address );
		}
		else if ( bank <= 0x1f && bank_offset >= 0x8000 && bank_offset <= 0xffff )
		{
			return rom[ address ];
		}
		else if ( bank >= 0x20 && bank <= 0x3f && bank_offset <= 0x1fff )
		{
			return wRam[ 0x1fff & address ];
		}
		else if ( bank >= 0x20 && bank <= 0x3f && bank_offset >= 0x8000 && bank_offset <= 0xffff )
		{
			return rom[ address - 0x200000 ];
		}
		else if ( bank >= 0x40 && bank <= 0x7d && bank_offset <= 0xffff )
		{
			return rom[ address - 0x400000 ];
		}
		else if ( bank >= 0x7e && bank <= 0x7f && bank_offset <= 0xffff )
		{
			return wRam[ address - 0x7e0000 ];
		}
		else if ( bank >= 0xc0 && bank <= 0xfd && bank_offset <= 0xffff )
		{
			return rom[ address - 0xc00000 ];
		}
		else if ( bank >= 0xfe && bank <= 0xff && bank_offset <= 0xffff )
		{
			return rom[ address - 0xc00000 ];
		}
		else if ( bank >= 0x80 && bank <= 0x9f && bank_offset <= 0x1fff )
		{
			return wRam[ 0x1fff & address ];
		}
		else if ( bank >= 0xa0 && bank <= 0xbf && bank_offset <= 0x1fff )
		{
			return wRam[ 0x1fff & address ];
		}

		return 0;
	}

	uint16_t load16( const uint32_t address )
	{
		uint16_t low = load8( address );
		uint16_t high  = load8( address + 1 );
		return ( high << 8 ) | low;
	}

	void store8( const uint32_t address, uint8_t value )
	{
		auto[ bank, bank_offset ] = getBankAndOffset( address );

		if ( bank <= 0x1f && bank_offset <= 0x1fff )
		{
			wRam[ 0x1fff & address ] = value;
		}
		else if ( ( bank <= 0x3f || ( bank >= 0x80 && bank <= 0xbf ) ) && bank_offset >= 0x2140 && bank_offset <= 0x217F )
		{
			spcWritePort( bank_offset & 0x3, value );
		}
		else if ( bank <= 0x3f && ( bank_offset >= 0x6000 && bank_offset <= 0x7001 ) )
		{
			dspWrite( address, value );
		}
		else if ( bank >= 0x20 && bank <= 0x3f && bank_offset <= 0x1fff )
		{
			wRam[ 0x1fff & address ] = value;
		}
		else if ( bank >= 0x7e && bank <= 0x7f && bank_offset <= 0xffff )
		{
			wRam[ address - 0x7e0000 ] = value;
		}
		else if ( bank >= 0x80 && bank <= 0x9f && bank_offset <= 0x1fff )
		{
			wRam[ 0x1fff & address ] = value;
		}
		else if ( bank >= 0xa0 && bank <= 0xbf && bank_offset <= 0x1fff )
		{
			wRam[ 0x1fff & address ] = value;
		}
	}

	void store16( const uint32_t address, uint16_t value )
	{
		store8( address, static_cast<uint8_t>( value & 0xff ) );
		store8( address + 1, static_cast<uint8_t>( value >> 8 ) );
	}
}

void LoadLog( const char* logPath )
{
	std::ifstream infile( logPath );
	std::string line;
	while ( std::getline( infile, line ) )
	{
		if ( line == "" )
		{
			continue;
		}

		int16_t a = static_cast<int16_t>( strtol( line.substr( line.find( "A:" ) + 2, 4 ).c_str(), NULL, 16 ) );
		int16_t x = static_cast<int16_t>( strtol( line.substr( line.find( "X:" ) + 2, 4 ).c_str(), NULL, 16 ) );
		int16_t y = static_cast<int16_t>( strtol( line.substr( line.find( "Y:" ) + 2, 4 ).c_str(), NULL, 16 ) );
		int16_t dp = static_cast<int16_t>( strtol( line.substr( line.find( "D:" ) + 2, 4 ).c_str(), NULL, 16 ) );
		int8_t db = static_cast<int8_t>( strtol( line.substr( line.find( "DB:" ) + 3, 2 ).c_str(), NULL, 16 ) );
		int16_t sp = static_cast<int16_t>( strtol( line.substr( line.find( "S:" ) + 2, 4 ).c_str(), NULL, 16 ) );
		std::string p = line.substr( line.find( "P:" ) + 2, 9 );

		logState.push_back( { line, a, x, y, dp, db, sp, p } );
	}
}

void LoadRom( const char* romPath )
{
	FILE * pFile = fopen( romPath, "rb" );
	if ( pFile )
	{
		auto result = fread( rom, 1, sizeof( rom ), pFile );
		assert( result == sizeof( rom ) );
		if ( result < sizeof( rom ) )
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

int main( int argc, char** argv ) 
{	
	snesSPC.init();
	snesSPC.init_rom( iplRom );
	if ( SDL_Init( SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER ) != 0 )
	{
		std::cout << "Error: " << SDL_GetError() << std::endl;
		return EXIT_FAILURE;
	}
	const char* glsl_version = "#version 130";
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_FLAGS, 0 );
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE );
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_MAJOR_VERSION, 3 );
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_MINOR_VERSION, 0 );

	SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );
	SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, 24 );
	SDL_GL_SetAttribute( SDL_GL_STENCIL_SIZE, 8 );
	SDL_WindowFlags window_flags = (SDL_WindowFlags)( SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI );
	window = SDL_CreateWindow( "SMK", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1600, 960, window_flags );
	gl_context = SDL_GL_CreateContext( window );
	SDL_GL_SetSwapInterval( 1 ); // Enable vsync

	int err = gl3wInit();
	if ( err != GL3W_OK )
	{
		std::cout << "Failed to initialize OpenGL loader!\n" << std::endl;
		return EXIT_FAILURE;
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGui::StyleColorsDark();

	ImGui_ImplSDL2_InitForOpenGL( window, gl_context );
	ImGui_ImplOpenGL3_Init( glsl_version );
	SDL_GL_MakeCurrent( window, gl_context );

	LoadRom( "Super Mario Kart (USA).sfc" );
	memset( wRam, 0x55, sizeof(wRam) );
	if constexpr ( compareToDebugLog )
	{
		LoadLog( "Super Mario Kart (USA)0000.log" );
	}
	snesSPC.reset();
	snesSPC.end_frame( 1024000 / 2 );

	ResetDSP();

	start();

	quit();
	return 0;
}