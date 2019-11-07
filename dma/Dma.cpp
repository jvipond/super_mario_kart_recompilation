#include "Dma.hpp"
#include "../smk_main.hpp"

static constexpr uint8_t TransferByteCount[ 8 ] = { 1, 2, 2, 4, 4, 4, 2, 4 };
static constexpr uint8_t TransferOffset[ 8 ][ 4 ] = {
	{ 0, 0, 0, 0 }, { 0, 1, 0, 1 }, { 0, 0, 0, 0 }, { 0, 0, 1, 1 },
	{ 0, 1, 2, 3 }, { 0, 1, 0, 1 }, { 0, 0, 0, 0 }, { 0, 0, 1, 1 }
};

DmaController::DmaController()
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

void DmaController::RunDma( DmaChannelConfig& channel )
{
	if ( !channel.m_DmaActive ) {
		return;
	}

	const uint8_t *transferOffsets = TransferOffset[ channel.m_TransferMode ];

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
		const uint8_t valToWrite = load8( addressBusB );
		store8( addressBusA, valToWrite );
	}
	else
	{
		const uint8_t valToWrite = load8( addressBusA );
		store8( addressBusB, valToWrite );
	}
}