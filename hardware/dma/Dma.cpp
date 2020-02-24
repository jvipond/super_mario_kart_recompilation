#include "Dma.hpp"
#include "../hardware.hpp"

static constexpr uint8_t TransferOffsetTable[ 8 ][ 4 ] = {
	{ 0, 0, 0, 0 }, { 0, 1, 0, 1 }, { 0, 0, 0, 0 }, { 0, 0, 1, 1 },
	{ 0, 1, 2, 3 }, { 0, 1, 0, 1 }, { 0, 0, 0, 0 }, { 0, 0, 1, 1 }
};

DmaController::DmaController()
: m_hdmaChannels( 0 )
{
	for ( auto& c : m_Channels )
	{
		c.m_DmaActive = false;
	}
}

DmaController::~DmaController()
{

}

void DmaController::Write( const uint32_t address, const uint8_t value )
{
	{
		switch ( address ) 
		{
		case 0x420B: {
			//MDMAEN - DMA Enable
			for ( int i = 0; i < 8; i++ ) {
				if ( value & ( 1 << i ) ) {
					m_Channels[ i ].m_DmaActive = true;
					RunDma( m_Channels[ i ] );
				}
			}
			break;
		}

		case 0x420C:
			m_hdmaChannels = value;
			break;

		case 0x4300: case 0x4310: case 0x4320: case 0x4330: case 0x4340: case 0x4350: case 0x4360: case 0x4370:
		{
			//DMAPx - DMA Control for Channel x
			DmaChannelConfig& channel = m_Channels[ ( address & 0x70 ) >> 4 ];
			channel.m_InvertDirection = ( value & 0x80 ) != 0;
			channel.m_HdmaIndirectAddressing = ( value & 0x40 ) != 0;
			channel.m_UnusedFlag = ( value & 0x20 ) != 0;
			channel.m_Decrement = ( value & 0x10 ) != 0;
			channel.m_FixedTransfer = ( value & 0x08 ) != 0;
			channel.m_TransferMode = value & 0x07;
			break;
		}

		case 0x4301: case 0x4311: case 0x4321: case 0x4331: case 0x4341: case 0x4351: case 0x4361: case 0x4371:
		{
			//BBADx - DMA Destination Register for Channel x
			DmaChannelConfig& channel = m_Channels[ ( address & 0x70 ) >> 4 ];
			channel.m_DestAddress = value;
			break;
		}

		case 0x4302: case 0x4312: case 0x4322: case 0x4332: case 0x4342: case 0x4352: case 0x4362: case 0x4372:
		{
			DmaChannelConfig& channel = m_Channels[ ( address & 0x70 ) >> 4 ];
			channel.m_SrcAddress = ( channel.m_SrcAddress & 0xFF00 ) | value;
			break;
		}

		case 0x4303: case 0x4313: case 0x4323: case 0x4333: case 0x4343: case 0x4353: case 0x4363: case 0x4373:
		{
			DmaChannelConfig& channel = m_Channels[ ( address & 0x70 ) >> 4 ];
			channel.m_SrcAddress = ( channel.m_SrcAddress & 0xFF ) | ( value << 8 );
			break;
		}

		case 0x4304: case 0x4314: case 0x4324: case 0x4334: case 0x4344: case 0x4354: case 0x4364: case 0x4374:
		{
			DmaChannelConfig& channel = m_Channels[ ( address & 0x70 ) >> 4 ];
			channel.m_SrcBank = value;
			break;
		}

		case 0x4305: case 0x4315: case 0x4325: case 0x4335: case 0x4345: case 0x4355: case 0x4365: case 0x4375:
		{
			//DASxL - DMA Size / HDMA Indirect Address low byte(x = 0 - 7)
			DmaChannelConfig& channel = m_Channels[ ( address & 0x70 ) >> 4 ];
			channel.m_TransferSize = ( channel.m_TransferSize & 0xFF00 ) | value;
			break;
		}

		case 0x4306: case 0x4316: case 0x4326: case 0x4336: case 0x4346: case 0x4356: case 0x4366: case 0x4376:
		{
			//DASxL - DMA Size / HDMA Indirect Address low byte(x = 0 - 7)
			DmaChannelConfig& channel = m_Channels[ ( address & 0x70 ) >> 4 ];
			channel.m_TransferSize = ( channel.m_TransferSize & 0xFF ) | ( value << 8 );
			break;
		}

		case 0x4307: case 0x4317: case 0x4327: case 0x4337: case 0x4347: case 0x4357: case 0x4367: case 0x4377:
		{
			//DASBx - HDMA Indirect Address bank byte (x=0-7)
			DmaChannelConfig& channel = m_Channels[ ( address & 0x70 ) >> 4 ];
			channel.m_HdmaBank = value;
			break;
		}

		case 0x4308: case 0x4318: case 0x4328: case 0x4338: case 0x4348: case 0x4358: case 0x4368: case 0x4378:
		{
			//A2AxL - HDMA Table Address low byte (x=0-7)
			DmaChannelConfig& channel = m_Channels[ ( address & 0x70 ) >> 4 ];
			channel.m_HdmaTableAddress = ( channel.m_HdmaTableAddress & 0xFF00 ) | value;
			break;
		}

		case 0x4309: case 0x4319: case 0x4329: case 0x4339: case 0x4349: case 0x4359: case 0x4369: case 0x4379:
		{
			//A2AxH - HDMA Table Address high byte (x=0-7)
			DmaChannelConfig& channel = m_Channels[ ( address & 0x70 ) >> 4 ];
			channel.m_HdmaTableAddress = ( value << 8 ) | ( channel.m_HdmaTableAddress & 0xFF );
			break;
		}

		case 0x430A: case 0x431A: case 0x432A: case 0x433A: case 0x434A: case 0x435A: case 0x436A: case 0x437A:
		{
			//DASBx - HDMA Indirect Address bank byte (x=0-7)
			DmaChannelConfig& channel = m_Channels[ ( address & 0x70 ) >> 4 ];
			channel.m_HdmaLineCounterAndRepeat = value;
			break;
		}
		}
	}
}

uint8_t DmaController::Read( const uint32_t address )
{
	switch ( address )
	{
	case 0x4300: case 0x4310: case 0x4320: case 0x4330: case 0x4340: case 0x4350: case 0x4360: case 0x4370:
	{
		//DMAPx - DMA Control for Channel x
		DmaChannelConfig& channel = m_Channels[ ( address & 0x70 ) >> 4 ];
		return (
			( channel.m_InvertDirection ? 0x80 : 0 ) |
			( channel.m_HdmaIndirectAddressing ? 0x40 : 0 ) |
			( channel.m_UnusedFlag ? 0x20 : 0 ) |
			( channel.m_Decrement ? 0x10 : 0 ) |
			( channel.m_FixedTransfer ? 0x08 : 0 ) |
			( channel.m_TransferMode & 0x07 )
			);
	}

	case 0x4301: case 0x4311: case 0x4321: case 0x4331: case 0x4341: case 0x4351: case 0x4361: case 0x4371:
	{
		//BBADx - DMA Destination Register for Channel x
		DmaChannelConfig& channel = m_Channels[ ( address & 0x70 ) >> 4 ];
		return channel.m_DestAddress;
	}

	case 0x4302: case 0x4312: case 0x4322: case 0x4332: case 0x4342: case 0x4352: case 0x4362: case 0x4372:
	{
		DmaChannelConfig& channel = m_Channels[ ( address & 0x70 ) >> 4 ];
		return channel.m_SrcAddress & 0xFF;
	}

	case 0x4303: case 0x4313: case 0x4323: case 0x4333: case 0x4343: case 0x4353: case 0x4363: case 0x4373:
	{
		DmaChannelConfig& channel = m_Channels[ ( address & 0x70 ) >> 4 ];
		return ( channel.m_SrcAddress >> 8 ) & 0xFF;
	}

	case 0x4304: case 0x4314: case 0x4324: case 0x4334: case 0x4344: case 0x4354: case 0x4364: case 0x4374:
	{
		DmaChannelConfig& channel = m_Channels[ ( address & 0x70 ) >> 4 ];
		return channel.m_SrcBank;
	}

	case 0x4305: case 0x4315: case 0x4325: case 0x4335: case 0x4345: case 0x4355: case 0x4365: case 0x4375:
	{
		//DASxL - DMA Size / HDMA Indirect Address low byte(x = 0 - 7)
		DmaChannelConfig& channel = m_Channels[ ( address & 0x70 ) >> 4 ];
		return channel.m_TransferSize & 0xFF;
	}

	case 0x4306: case 0x4316: case 0x4326: case 0x4336: case 0x4346: case 0x4356: case 0x4366: case 0x4376:
	{
		//DASxL - DMA Size / HDMA Indirect Address low byte(x = 0 - 7)
		DmaChannelConfig& channel = m_Channels[ ( address & 0x70 ) >> 4 ];
		return ( channel.m_TransferSize >> 8 ) & 0xFF;
	}

	case 0x4307: case 0x4317: case 0x4327: case 0x4337: case 0x4347: case 0x4357: case 0x4367: case 0x4377:
	{
		//DASBx - HDMA Indirect Address bank byte (x=0-7)
		DmaChannelConfig& channel = m_Channels[ ( address & 0x70 ) >> 4 ];
		return channel.m_HdmaBank;
	}

	case 0x4308: case 0x4318: case 0x4328: case 0x4338: case 0x4348: case 0x4358: case 0x4368: case 0x4378:
	{
		//A2AxL - HDMA Table Address low byte (x=0-7)
		DmaChannelConfig& channel = m_Channels[ ( address & 0x70 ) >> 4 ];
		return channel.m_HdmaTableAddress & 0xFF;
	}

	case 0x4309: case 0x4319: case 0x4329: case 0x4339: case 0x4349: case 0x4359: case 0x4369: case 0x4379:
	{
		//A2AxH - HDMA Table Address high byte (x=0-7)
		DmaChannelConfig& channel = m_Channels[ ( address & 0x70 ) >> 4 ];
		return ( channel.m_HdmaTableAddress >> 8 ) & 0xFF;
	}

	case 0x430A: case 0x431A: case 0x432A: case 0x433A: case 0x434A: case 0x435A: case 0x436A: case 0x437A:
	{
		//DASBx - HDMA Indirect Address bank byte (x=0-7)
		DmaChannelConfig& channel = m_Channels[ ( address & 0x70 ) >> 4 ];
		return channel.m_HdmaLineCounterAndRepeat;
	}
	}
	return 0;
}

void DmaController::RunHdmaTransfer( DmaChannelConfig& channel )
{
	static constexpr uint8_t transferByteCountTable[ 8 ] = { 1, 2, 2, 4, 4, 4, 2, 4 };
	const uint8_t *transferOffsets = TransferOffsetTable[ channel.m_TransferMode ];
	uint8_t transferByteCount = transferByteCountTable[ channel.m_TransferMode ];
	channel.m_DmaActive = false;

	uint8_t i = 0;
	if ( channel.m_HdmaIndirectAddressing ) {
		do {
			CopyDmaByte(
				( channel.m_HdmaBank << 16 ) | channel.m_TransferSize,
				0x2100 | ( channel.m_DestAddress + transferOffsets[ i ] ),
				channel.m_InvertDirection
			);
			channel.m_TransferSize++;
			i++;
		} while ( i < transferByteCount );
	}
	else {
		do {
			CopyDmaByte(
				( channel.m_SrcBank << 16 ) | channel.m_HdmaTableAddress,
				0x2100 | ( channel.m_DestAddress + transferOffsets[ i ] ),
				channel.m_InvertDirection
			);
			channel.m_HdmaTableAddress++;
			i++;
		} while ( i < transferByteCount );
	}
}

void DmaController::ProcessHDMAChannels()
{
	if ( m_hdmaChannels == 0 )
	{
		return;
	}

	//Run all the DMA transfers for each channel first, before fetching data for the next scanline
	for ( int i = 0; i < 8; i++ ) {
		DmaChannelConfig &ch = m_Channels[ i ];
		if ( ( m_hdmaChannels & ( 1 << i ) ) == 0 ) {
			continue;
		}

		ch.m_DmaActive = false;

		if ( ch.m_HdmaFinished ) {
			continue;
		}

		//1. If DoTransfer is false, skip to step 3.
		if ( ch.m_DoTransfer ) {
			//2. For the number of bytes (1, 2, or 4) required for this Transfer Mode...
			RunHdmaTransfer( ch );
		}
	}

	//Update the channel's state & fetch data for the next scanline
	for ( int i = 0; i < 8; i++ ) {
		DmaChannelConfig &ch = m_Channels[ i ];
		if ( ( m_hdmaChannels & ( 1 << i ) ) == 0 || ch.m_HdmaFinished ) {
			continue;
		}

		//3. Decrement $43xA.
		ch.m_HdmaLineCounterAndRepeat--;

		//4. Set DoTransfer to the value of Repeat.
		ch.m_DoTransfer = ( ch.m_HdmaLineCounterAndRepeat & 0x80 ) != 0;

		//"a. Read the next byte from Address into $43xA (thus, into both Line Counter and Repeat)."
		//This value is discarded if the line counter isn't 0
		uint8_t newCounter = read8( ( ch.m_SrcBank << 16 ) | ch.m_HdmaTableAddress );

		//5. If Line Counter is zero...
		if ( ( ch.m_HdmaLineCounterAndRepeat & 0x7F ) == 0 ) {
			ch.m_HdmaLineCounterAndRepeat = newCounter;
			ch.m_HdmaTableAddress++;

			//"b. If Addressing Mode is Indirect, read two bytes from Address into Indirect Address(and increment Address by two bytes)."
			if ( ch.m_HdmaIndirectAddressing ) {
				if ( ch.m_HdmaLineCounterAndRepeat == 0 ) {
					//"One oddity: if $43xA is 0 and this is the last active HDMA channel for this scanline, only load one byte for Address, 
					//and use the $00 for the low byte.So Address ends up incremented one less than otherwise expected, and one less CPU Cycle is used."
					uint8_t msb = read8( ( ch.m_SrcBank << 16 ) | ch.m_HdmaTableAddress++ );
					ch.m_TransferSize = ( msb << 8 );
				}
				else {
					//"If a new indirect address is required, 16 master cycles are taken to load it."
					uint8_t lsb = read8( ( ch.m_SrcBank << 16 ) | ch.m_HdmaTableAddress++ );

					uint8_t msb = read8( ( ch.m_SrcBank << 16 ) | ch.m_HdmaTableAddress++ );

					ch.m_TransferSize = ( msb << 8 ) | lsb;
				}
			}

			//"c. If $43xA is zero, terminate this HDMA channel for this frame. The bit in $420c is not cleared, though, so it may be automatically restarted next frame."
			if ( ch.m_HdmaLineCounterAndRepeat == 0 ) {
				ch.m_HdmaFinished = true;
			}

			//"d. Set DoTransfer to true."
			ch.m_DoTransfer = true;
		}
	}
}

void DmaController::InitHDMAChannels()
{
	for ( int i = 0; i < 8; i++ ) {
		//Reset internal flags on every frame, whether or not the channels are enabled
		m_Channels[ i ].m_HdmaFinished = false;
		m_Channels[ i ].m_DoTransfer = false; //not resetting this causes graphical glitches in some games (Aladdin, Super Ghouls and Ghosts)
	}

	if ( m_hdmaChannels == 0 ) {
		//No channels are enabled, no more processing needs to be done
		return;
	}

	for ( int i = 0; i < 8; i++ ) {
		DmaChannelConfig &ch = m_Channels[ i ];

		//Set DoTransfer to true for all channels if any HDMA channel is enabled
		ch.m_DoTransfer = true;

		if ( m_hdmaChannels & ( 1 << i ) ) {
			//"1. Copy AAddress into Address."
			ch.m_HdmaTableAddress = ch.m_SrcAddress;
			ch.m_DmaActive = false;

			//"2. Load $43xA (Line Counter and Repeat) from the table. I believe $00 will terminate this channel immediately."
			ch.m_HdmaLineCounterAndRepeat = read8( ( ch.m_SrcBank << 16 ) | ch.m_HdmaTableAddress );

			ch.m_HdmaTableAddress++;
			if ( ch.m_HdmaLineCounterAndRepeat == 0 ) {
				ch.m_HdmaFinished = true;
			}

			//3. Load Indirect Address, if necessary.
			if ( ch.m_HdmaIndirectAddressing ) {
				uint8_t lsb = read8( ( ch.m_SrcBank << 16 ) | ch.m_HdmaTableAddress++ );
				uint8_t msb = read8( ( ch.m_SrcBank << 16 ) | ch.m_HdmaTableAddress++ );
				ch.m_TransferSize = ( msb << 8 ) | lsb;
			}
		}
	}

}

void DmaController::RunDma( DmaChannelConfig& channel )
{
	if ( !channel.m_DmaActive ) {
		return;
	}

	const uint8_t *transferOffsets = TransferOffsetTable[ channel.m_TransferMode ];

	uint8_t i = 0;
	do {
		//Manual DMA transfers run to the end of the transfer when started
		CopyDmaByte(
			( channel.m_SrcBank << 16 ) | channel.m_SrcAddress,
			0x2100 | ( channel.m_DestAddress + transferOffsets[ i & 0x03 ] ),
			channel.m_InvertDirection
		);

		if ( !channel.m_FixedTransfer ) {
			channel.m_SrcAddress += channel.m_Decrement ? -1 : 1;
		}

		channel.m_TransferSize--;
		i++;
	} while ( channel.m_TransferSize > 0 && channel.m_DmaActive );

	channel.m_DmaActive = false;
}

void DmaController::CopyDmaByte( uint32_t addressBusA, uint16_t addressBusB, bool fromBtoA )
{
	if ( fromBtoA ) 
	{
		const uint8_t valToWrite = read8( addressBusB );
		write8( addressBusA, valToWrite );
	}
	else
	{
		const uint8_t valToWrite = read8( addressBusA );
		write8( addressBusB, valToWrite );
	}
}