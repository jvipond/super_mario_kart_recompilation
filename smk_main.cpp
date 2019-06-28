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

	void start( const int32_t interrupt );
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

	std::vector<std::tuple<uint32_t, const char*, uint32_t>> instructionTrace;

	void updateInstructionOutput( const uint32_t pc, const char* instructionString )
	{
		instructionTrace.push_back( { pc, instructionString, 1 } );
	}

	static bool autoStep = false;
	static MemoryEditor mem_edit;
	void romCycle( const int32_t cycles, const uint32_t implemented )
	{
		std::get<2>( instructionTrace.back() ) = implemented;
		ImGuiIO& io = ImGui::GetIO();

		bool show_demo_window = false;
		bool show_another_window = false;
		ImVec4 clear_color = ImVec4( 0.45f, 0.55f, 0.60f, 1.00f );

		bool scrollToBottom = true;
		bool done = false;
		while ( !done )
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
				for ( auto& [ pc, instructionString, hasBeenImplemented ] : instructionTrace )
				{
					ImGui::Text( "$%06X %s ", pc, instructionString );
					ImGui::SameLine();
					ImGui::TextColored( ImVec4( 1.0f, 0.0f, 1.0f, 1.0f ), "%s", hasBeenImplemented == 0 ? "[NOT IMPLEMENTED]" : "" );
				}
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
}

enum InterruptVector 
{
	INTERRUPT_VECTOR_ROM_RESET = 1,
	INTERRUPT_VECTOR_NMI,
	INTERRUPT_VECTOR_IRQ,
};

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
	window = SDL_CreateWindow( "SMK", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, window_flags );
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
	start( INTERRUPT_VECTOR_ROM_RESET );

	quit();
	return 0;
}