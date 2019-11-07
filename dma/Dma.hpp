#ifndef DMA_HPP
#define DMA_HPP

#include <array>

class DmaController
{
	public:
		DmaController();
		~DmaController();

		void Write( const uint32_t address, const uint8_t value );
		uint8_t Read( const uint32_t address );

	private:
		struct DmaChannelConfig
		{
			bool m_DmaActive = false;

			bool m_InvertDirection = false;
			bool m_Decrement = false;
			bool m_FixedTransfer = false;
			bool m_HdmaIndirectAddressing = false;
			uint8_t m_TransferMode = 0;

			uint16_t m_SrcAddress = 0;
			uint8_t m_SrcBank = 0;

			uint16_t m_TransferSize = 0;
			uint8_t m_DestAddress = 0;

			uint16_t m_HdmaTableAddress = 0;
			uint8_t m_HdmaBank = 0;
			uint8_t m_HdmaLineCounterAndRepeat = 0;
			bool m_DoTransfer = false;
			bool m_HdmaFinished = false;

			bool m_UnusedFlag = false;
		};

		void RunDma( DmaChannelConfig& channel );
		void CopyDmaByte( uint32_t addressBusA, uint16_t addressBusB, bool fromBtoA );
		std::array< DmaChannelConfig, 8 > m_Channels;
};

#endif // DMA_HPP