#ifndef HARDWARE_HPP
#define HARDWARE_HPP

#include <cstdint>
#include <tuple>
#include <vector>
#include "SDL_video.h"
#include "../imgui/imgui.h"
#include "../imgui/imgui_memory_editor.h"
#include <deque>
#include "spc/SNES_SPC.h"
#include "dsp/dsp.h"
#include "dma/Dma.hpp"

std::tuple<uint32_t, uint32_t> getBankAndOffset( uint32_t addr );

extern "C"
{
	uint8_t ADC8( uint8_t data );
	uint16_t ADC16( uint16_t data );
	uint8_t SBC8( uint8_t data );
	uint16_t SBC16( uint16_t data );
	void start( void );
	void mainLoop( void );
	void panic( void );

	uint8_t read8( const uint32_t address );
	void write8( const uint32_t address, const uint8_t value );
	void doPPUFrame( void );
	void updateInstructionOutput( const uint32_t pc, const char* instructionString );
	void romCycle( const int32_t cycles, const uint32_t implemented );
}

struct InternalRegisterState
{
	bool enableAutoJoypadRead = false;

	bool enableNmi = true;
	bool enableHorizontalIrq = false;
	bool enableVerticalIrq = false;
	uint16_t horizontalTimer = 0;
	uint16_t verticalTimer = 0;

	uint8_t ioPortOutput = 0;

	uint16_t controllerData[ 4 ] = { 0 };
};

void mainLoopFunc( void );

class Hardware
{
	public:
		static Hardware& GetInstance();
		void PowerOn();
		void quit();
		void mainLoopFunc();

	struct AluMulDivState
	{
		//$4202-$4203
		uint8_t wrmpya = 0xff;
		uint8_t wrmpyb = 0xff;

		//$4204-$4206
		uint16_t wrdiva = 0xffff;
		uint8_t wrdivb = 0xff;

		//$4214-$4217
		uint16_t rddiv = 0;
		uint16_t rdmpy = 0;
	};

	void incrementCycleCount( void );
	void spcWritePort( const int32_t port, const int32_t data );
	int32_t spcReadPort( const int32_t port );

	class SnesController
	{
	public:
		SnesController( const uint8_t port );
		bool IsCurrentPort( const uint32_t address );
		void RefreshStateBuffer();
		uint8_t read( const uint32_t address );
		void write( const uint32_t address, const uint8_t data );
		void UpdateKeyboardState();

	private:
		bool m_Strobe = false;
		uint8_t m_Port = 0;
		uint32_t m_StateBuffer = 0;
		const uint8_t* m_KeyboardState = nullptr;
	};

	void ProcessAutoJoyPadRead( void );
	
	uint8_t read8( const uint32_t address );
	void write8( const uint32_t address, const uint8_t value );
	void DoPPUFrame();
	void UpdateInstructionOutput( const uint32_t pc, const char* instructionString );
	void Panic();
	void RomCycle( const int32_t cycles, const uint32_t implemented );

private:
	Hardware() {};
	Hardware( Hardware const& other ) = delete;
	Hardware( Hardware&& other ) = delete;

	void initialiseSDL();
	void LoadRom( const char* romPath );

	uint8_t dspRead( const uint32_t addr );
	void dspWrite( const uint32_t addr, const uint8_t data );

	static constexpr uint8_t IPL_ROM[] = { 0xCD, 0xEF, 0xBD, 0xE8, 0x00, 0xC6, 0x1D, 0xD0, 0xFC, 0x8F, 0xAA, 0xF4, 0x8F, 0xBB, 0xF5, 0x78,
																				 0xCC, 0xF4, 0xD0, 0xFB, 0x2F, 0x19, 0xEB, 0xF4, 0xD0, 0xFC, 0x7E, 0xF4, 0xD0, 0x0B, 0xE4, 0xF5,
																				 0xCB, 0xF4, 0xD7, 0x00, 0xFC, 0xD0, 0xF3, 0xAB, 0x01, 0x10, 0xEF, 0x7E, 0xF4, 0x10, 0xEB, 0xBA,
																				 0xF6, 0xDA, 0x00, 0xBA, 0xF4, 0xC4, 0xF4, 0xDD, 0x5D, 0xD0, 0xDB, 0x1F, 0x00, 0x00, 0xC0, 0xFF };

	int8_t m_wRam[ 0x20000 ] = { 0 };
	int8_t m_rom[ 0x80000 ] = { 0 };
	int8_t m_sRam[ 0x800 ] = { 0 };

	SDL_Window* m_Window;
	SDL_GLContext m_GLContext;

	std::vector<SnesController> m_SnesControllers;
	AluMulDivState m_aluMulDivState;
	InternalRegisterState m_InternalRegisterState;
	SNES_SPC m_SPC;
	int32_t m_SPCTime = 0;

	uint32_t m_wRamPosition = 0;
	DmaController m_dmaController;

	bool m_AutoStep = true;
	bool m_DoRender = false;
	bool m_RenderSnesOutputToScreen = true;

	struct RegisterState
	{
		uint16_t A = 0;
		uint8_t DB = 0;
		uint16_t DP = 0;
		uint8_t PB = 0;
		uint16_t PC = 0;
		uint16_t SP = 0;
		uint16_t X = 0;
		uint16_t Y = 0;

		bool CF = false;
		bool ZF = false;
		bool IF = false;
		bool DF = false;
		bool XF = false;
		bool MF = false;
		bool VF = false;
		bool NF = false;
		bool EF = true;
	};

	std::deque<std::tuple<uint32_t, const char*, RegisterState, uint32_t>> m_InstructionTrace;

	MemoryEditor m_MemoryEditor;
};

#endif // HARDWARE_HPP