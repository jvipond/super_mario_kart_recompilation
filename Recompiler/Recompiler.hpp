#ifndef RECOMPILER_HPP
#define RECOMPILER_HPP

#include <map>
#include <string>
#include <vector>
#include <variant>
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"

class Recompiler
{
public:
	Recompiler();
	~Recompiler();

	void LoadAST( const char* filename );
	void Recompile();

	void AddLabelNameToBasicBlock( const std::string& labelName, llvm::BasicBlock* basicBlock );
	void InitialiseBasicBlocksFromLabelNames();
	void AddDynamicJumpTableBlock();
	void GenerateCode();
	void AddOffsetToInstructionString( const uint32_t offset, const std::string& stringGlobalVariable );
	void AddInstructionStringGlobalVariables();

	const std::map< std::string, llvm::BasicBlock* >& GetLabelNamesToBasicBlocks( void ) const { return m_LabelNamesToBasicBlocks; }
	const llvm::BasicBlock* GetCurrentBasicBlock( void ) const { return m_CurrentBasicBlock; }
	void SetCurrentBasicBlock( llvm::BasicBlock* basicBlock ) { m_CurrentBasicBlock = basicBlock; }

	void SelectBlock( llvm::BasicBlock* basicBlock );

	llvm::BasicBlock* CreateBlock( const char* name );
	llvm::BasicBlock* CreateIf( llvm::Value* cond );
	void CreateBranch( llvm::BasicBlock* basicBlock );
	void SetInsertPoint( llvm::BasicBlock* basicBlock );
	void PerformOra16( llvm::Value* value );
	void PerformOra8( llvm::Value* value );
	void PerformAnd16( llvm::Value* value );
	void PerformAnd8( llvm::Value* value );
	void PerformEor16( llvm::Value* value );
	void PerformEor8( llvm::Value* value );
	void PerformLda16( llvm::Value* value );
	void PerformLda8( llvm::Value* value );
	void PerformLdx16( llvm::Value* value );
	void PerformLdx8( llvm::Value* value );
	void PerformLdy16( llvm::Value* value );
	void PerformLdy8( llvm::Value* value );
	void PerformCmp16( llvm::Value* lValue, llvm::Value* rValue );
	void PerformCmp8( llvm::Value* lValue, llvm::Value* rValue );
	void PerformAdc16( llvm::Value* value );
	void PerformAdc8( llvm::Value* value );
	void PerformSbc16( llvm::Value* value );
	void PerformSbc8( llvm::Value* value );
	void PerformBit16( llvm::Value* value );
	void PerformBit8( llvm::Value* value );
	void TestAndSetZero16( llvm::Value* value );
	void TestAndSetZero8( llvm::Value* value );
	void TestAndSetNegative16( llvm::Value* value );
	void TestAndSetNegative8( llvm::Value* value );
	void TestAndSetCarrySubtraction( llvm::Value* lValue, llvm::Value* rValue );
	void PerformXba();
	void PerformTcs();
	void PerformTcd();
	void PerformTdc();
	void PerformTsc();
	void PerformRtl();
	void PerformRts();
	void PerformRti();
	void PerformPea( llvm::Value* value );
	void PerformBra( const std::string& labelName, llvm::Value* newPC );
	void PerformJmpAbs( const std::string& labelName, const uint32_t jumpAddress );
	void PerformJmpLng( const std::string& labelName, const uint32_t jumpAddress );
	void PerformJsr( const std::string& labelName, const uint32_t jumpAddress );
	void PerformJsl( const std::string& labelName, const uint32_t jumpAddress );
	void PerformBcc( const std::string& labelName, llvm::Value* pcBranchTaken, llvm::Value* pcBranchNotTaken );
	void PerformBcs( const std::string& labelName, llvm::Value* pcBranchTaken, llvm::Value* pcBranchNotTaken );
	void PerformBeq( const std::string& labelName, llvm::Value* pcBranchTaken, llvm::Value* pcBranchNotTaken );
	void PerformBne( const std::string& labelName, llvm::Value* pcBranchTaken, llvm::Value* pcBranchNotTaken );
	void PerformBmi( const std::string& labelName, llvm::Value* pcBranchTaken, llvm::Value* pcBranchNotTaken );
	void PerformBpl( const std::string& labelName, llvm::Value* pcBranchTaken, llvm::Value* pcBranchNotTaken );
	void PerformBvc( const std::string& labelName, llvm::Value* pcBranchTaken, llvm::Value* pcBranchNotTaken );
	void PerformBvs( const std::string& labelName, llvm::Value* pcBranchTaken, llvm::Value* pcBranchNotTaken );
	void AddConditionalBranch( llvm::Value* cond, const std::string& labelName, llvm::Value* pcBranchTaken, llvm::Value* pcBranchNotTaken );
	void PerformSep( llvm::Value* value );
	void PerformRep( llvm::Value* value );
	void PerformPhb( void );
	void PerformPhd( void );
	void PerformPhk( void );
	void PerformPhp( void );
	void PerformPlb( void );
	void PerformPld( void );
	void PerformPlp( void );
	void PerformInc( void );
	llvm::Value* PerformInc16Acc( void );
	llvm::Value* PerformInc8Acc( void );
	void PerformDec( void );
	llvm::Value* PerformDec16Acc( void );
	llvm::Value* PerformDec8Acc( void );
	void PerformInx( void );
	llvm::Value* PerformInx16Acc( void );
	llvm::Value* PerformInx8Acc( void );
	void PerformDex( void );
	llvm::Value* PerformDex16Acc( void );
	llvm::Value* PerformDex8Acc( void );
	void PerformIny( void );
	llvm::Value* PerformIny16Acc( void );
	llvm::Value* PerformIny8Acc( void );
	void PerformDey( void );
	llvm::Value* PerformDey16Acc( void );
	llvm::Value* PerformDey8Acc( void );
	void PerformAsl( void );
	llvm::Value* PerformAsl16Acc( void );
	llvm::Value* PerformAsl8Acc( void );
	void PerformLsr( void );
	llvm::Value* PerformLsr16Acc( void );
	llvm::Value* PerformLsr8Acc( void );
	void PerformRol( void );
	llvm::Value* PerformRol16Acc( void );
	llvm::Value* PerformRol8Acc( void );
	void PerformRor( void );
	llvm::Value* PerformRor16Acc( void );
	llvm::Value* PerformRor8Acc( void );
	void PerformTax( void );
	void PerformTay( void );
	void PerformTsx( void );
	void PerformTxa( void );
	void PerformTxs( void );
	void PerformTxy( void );
	void PerformTya( void );
	void PerformTyx( void );
	void PerformRegisterTransfer( llvm::Value* sourceRegister, llvm::Value* destinationRegister );
	llvm::Value* PerformRegisterTransfer16( llvm::Value* sourceRegister, llvm::Value* destinationRegister );
	llvm::Value* PerformRegisterTransfer8( llvm::Value* sourceRegister, llvm::Value* destinationRegister );
	llvm::Value* ComputeNewPC( llvm::Value* payloadSize );
	void PerformRomCycle( llvm::Value* value );
	void PerformRomCycle( llvm::Value* value, llvm::Value* newPc );
	void PerformUpdateInstructionOutput( const uint32_t offset, const std::string& instructionString );
	void Panic( void );
	llvm::Value* PullByteFromStack();
	llvm::Value* PullWordFromStack();
	void PushByteToStack( llvm::Value* value );
	void PushWordToStack( llvm::Value* value );
	void ClearCarry();
	void SetCarry();
	void ClearDecimal();
	void SetDecimal();
	void ClearInterrupt();
	void SetInterrupt();
	void ClearOverflow();
	llvm::Value* CreateLoadA16( void );
	llvm::Value* CreateLoadA8( void );
	llvm::Value* CreateLoadB8( void );
	llvm::Value* CreateLoadX16( void );
	llvm::Value* CreateLoadX8( void );
	llvm::Value* CreateLoadY16( void );
	llvm::Value* CreateLoadY8( void );
	void AddEnterNmiInterruptCode( void );

private:
	class Label
	{
	public:
		Label( const std::string& name, const uint32_t offset );
		~Label();

		const std::string& GetName() const { return m_Name; }

	private:
		std::string m_Name;
		uint32_t m_Offset;
	};

	enum MemoryMode
	{
		SIXTEEN_BIT = 0,
		EIGHT_BIT = 1,
	};

	class Instruction
	{
	public:
		Instruction( const uint32_t offset, const std::string& instructionString, const uint8_t opcode, const uint32_t operand, const uint32_t operand_size, MemoryMode memoryMode, MemoryMode indexMode );
		Instruction( const uint32_t offset, const std::string& instructionString, const uint8_t opcode, const uint32_t operand, const std::string& jumpLabelName, const uint32_t operand_size, MemoryMode memoryMode, MemoryMode indexMode );
		Instruction( const uint32_t offset, const std::string& instructionString, const uint8_t opcode, MemoryMode memoryMode, MemoryMode indexMode );
		~Instruction();

		uint8_t GetOpcode( void ) const { return m_Opcode; }
		const std::string& GetInstructionString( void ) const { return m_InstructionString; }
		uint32_t GetOperand( void ) const { return m_Operand; }
		uint32_t GetOperandSize( void ) const { return m_OperandSize; }
		uint32_t GetTotalSize( void ) const { return m_OperandSize + 1; }
		bool HasOperand( void ) const { return m_HasOperand; }
		const MemoryMode& GetMemoryMode() const { return m_MemoryMode; }
		const MemoryMode& GetIndexMode() const { return m_IndexMode; }
		uint32_t GetOffset( void ) const { return m_Offset; }
		const std::string& GetJumpLabelName( void ) const { return m_JumpLabelName; }

	private:
		uint32_t m_Offset;
		std::string m_InstructionString;
		uint8_t m_Opcode;
		uint32_t m_Operand;
		std::string m_JumpLabelName;
		uint32_t m_OperandSize;
		MemoryMode m_MemoryMode;
		MemoryMode m_IndexMode;
		bool m_HasOperand;
	};


	llvm::LLVMContext m_LLVMContext;
	llvm::IRBuilder<> m_IRBuilder;
	llvm::Module m_RecompilationModule;

	std::string m_RomResetLabelName;
	uint32_t m_RomResetAddr;
	std::string m_RomNmiLabelName;
	std::string m_RomIrqLabelName;
	std::vector< std::variant<Label, Instruction> > m_Program;
	std::map< std::string, uint32_t > m_LabelNamesToOffsets;
	std::map< uint32_t, std::string > m_OffsetsToLabelNames;
	std::map< std::string, llvm::BasicBlock* > m_LabelNamesToBasicBlocks;
	std::map< uint32_t, llvm::BasicBlock* > m_DynamicJumpOffsetsToBasicBlocks;
	std::map< uint32_t, llvm::GlobalVariable* > m_OffsetsToInstructionStringGlobalVariable;

	llvm::BasicBlock* m_DynamicJumpTableBlock;
	llvm::Function* m_MainFunction;

	llvm::GlobalVariable m_registerA;
	llvm::GlobalVariable m_registerDB;
	llvm::GlobalVariable m_registerDP;
	llvm::GlobalVariable m_registerPB;
	llvm::GlobalVariable m_registerPC;
	llvm::GlobalVariable m_registerSP;
	llvm::GlobalVariable m_registerX;
	llvm::GlobalVariable m_registerY;
	llvm::GlobalVariable m_registerP;

	static const uint32_t WRAM_SIZE = 0x20000;
	llvm::GlobalVariable m_wRam;

	static const uint32_t ROM_SIZE = 0x80000;
	llvm::GlobalVariable m_Rom;

	llvm::BasicBlock* m_CurrentBasicBlock;
	llvm::Function* m_CycleFunction;
	llvm::Function* m_PanicFunction;
	llvm::BasicBlock* m_PanicBlock;

	llvm::Function* m_ConvertRuntimeAddressFunction;
	llvm::Function* m_UpdateInstructionOutput;
};

#endif // RECOMPILER_HPP