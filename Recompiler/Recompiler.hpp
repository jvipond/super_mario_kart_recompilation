#ifndef RECOMPILER_HPP
#define RECOMPILER_HPP

#include <map>
#include <string>
#include <vector>
#include <variant>
#include <set>
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"

class Recompiler
{
public:
	Recompiler();
	~Recompiler();

	void LoadAST( const std::string& filename );
	void Recompile( const std::string& targetType );

	void AddLabelNameToBasicBlock( const std::string& labelName, llvm::BasicBlock* basicBlock );
	void CreateFunctions();
	void InitialiseBasicBlocksFromLabelNames();
	void GenerateCode();
	void EnforceFunctionEntryBlocksConstraints();
	void SetupNmiCall();
	void SetupIrqFunction();
	void FixReturnAddressManipulationFunctions();
	void CreateMainLoopFunction();
	void AddOffsetToInstructionString( const uint32_t offset, const std::string& stringGlobalVariable );
	void AddInstructionStringGlobalVariables();
	void SelectBlock( llvm::BasicBlock* basicBlock );

	const std::unordered_map<uint32_t, std::unordered_map<std::string, bool> >& GetLabelsToFunctions() const { return m_LabelsToFunctions; }
	const std::unordered_map<std::string, llvm::Function*>& GetFunctions() const { return m_Functions; }

	void SetInsertPoint( llvm::BasicBlock* basicBlock );
	
	void PerformRomCycle( llvm::Value* value );
	void PerformUpdateInstructionOutput( const uint32_t offset, const uint32_t pc, const std::string& instructionString );

private:
	llvm::Value* CombineTo16( llvm::Value* low8, llvm::Value* high8 );
	llvm::Value* CombineTo32( llvm::Value* low8, llvm::Value* mid8, llvm::Value* high8 );
	auto Recompiler::ConvertTo8( llvm::Value* value16 );
	auto Recompiler::GetLowHighPtrFromPtr16( llvm::Value* ptr16 );

	llvm::Value* ReadDirect( llvm::Value* address );
	llvm::Value* ReadDirectNative( llvm::Value* address );
	void WriteDirect( llvm::Value* address, llvm::Value* value );
	llvm::Value* ReadBank( llvm::Value* address );
	void WriteBank( llvm::Value* address, llvm::Value* value );
	llvm::Value* ReadLong( llvm::Value* address );
	void WriteLong( llvm::Value* address, llvm::Value* value );
	llvm::Value* ReadStack( llvm::Value* address );
	void WriteStack( llvm::Value* address, llvm::Value* value );
	llvm::Value* Pull();
	llvm::Value* PullNative();
	void Push( llvm::Value* value8 );
	void PushNative( llvm::Value* value8 );

	llvm::Value* Read8( llvm::Value* address );
	void Write8( llvm::Value* address, llvm::Value* value );

	llvm::Value* LoadRegister32( llvm::Value* value );
	llvm::Value* CreateDirectAddress( llvm::Value* address );
	llvm::Value* CreateDirectEmulationAddress( llvm::Value* address );
	llvm::Value* CreateBankAddress( llvm::Value* address );
	llvm::Value* CreateStackAddress( llvm::Value* address );

	using Operation = llvm::Value* ( Recompiler::* )( llvm::Value* );

	void InstructionImpliedModify8( Operation op, llvm::Value* ptr16 );
	void InstructionImpliedModify16( Operation op, llvm::Value* ptr16 );
	void InstructionBankModify8( Operation op, llvm::Value* address32 );
	void InstructionBankModify16( Operation op, llvm::Value* address32 );
	void InstructionBankIndexedModify8( Operation op, llvm::Value* address16 );
	void InstructionBankIndexedModify16( Operation op, llvm::Value* address16 );
	void InstructionDirectModify8( Operation op, llvm::Value* address32 );
	void InstructionDirectModify16( Operation op, llvm::Value* address32 );
	void InstructionDirectIndexedModify8( Operation op, llvm::Value* address16 );
	void InstructionDirectIndexedModify16( Operation op, llvm::Value* address16 );
	
	void InstructionImmediateRead8( Operation op, llvm::Value* operand8 );
	void InstructionImmediateRead16( Operation op, llvm::Value* operand16 );
	void InstructionBankRead8( Operation op, llvm::Value* address32 );
	void InstructionBankRead16( Operation op, llvm::Value* address32 );
	void InstructionBankRead8( Operation op, llvm::Value* address16, llvm::Value* I16 );
	void InstructionBankRead16( Operation op, llvm::Value* address16, llvm::Value* I16 );
	void InstructionLongRead8( Operation op, llvm::Value* address32, llvm::Value* I16 );
	void InstructionLongRead16( Operation op, llvm::Value* address32, llvm::Value* I16 );
	void InstructionDirectRead8( Operation op, llvm::Value* address32 );
	void InstructionDirectRead16( Operation op, llvm::Value* address32 );
	void InstructionDirectRead8( Operation op, llvm::Value* address16, llvm::Value* I16 );
	void InstructionDirectRead16( Operation op, llvm::Value* address16, llvm::Value* I16 );
	void InstructionIndirectRead8( Operation op, llvm::Value* address32 );
	void InstructionIndirectRead16( Operation op, llvm::Value* address32 );
	void InstructionIndexedIndirectRead8( Operation op, llvm::Value* address32 );
	void InstructionIndexedIndirectRead16( Operation op, llvm::Value* address32 );
	void InstructionIndirectIndexedRead8( Operation op, llvm::Value* address32 );
	void InstructionIndirectIndexedRead16( Operation op, llvm::Value* address32 );
	void InstructionIndirectLongRead8( Operation op, llvm::Value* address32, llvm::Value* I16 );
	void InstructionIndirectLongRead16( Operation op, llvm::Value* address32, llvm::Value* I16 );
	void InstructionStackRead8( Operation op, llvm::Value* address32 );
	void InstructionStackRead16( Operation op, llvm::Value* address32 );
	void InstructionIndirectStackRead8( Operation op, llvm::Value* address32 );
	void InstructionIndirectStackRead16( Operation op, llvm::Value* address32 );

	void InstructionBankWrite8( llvm::Value* address32, llvm::Value* value );
	void InstructionBankWrite16( llvm::Value* address32, llvm::Value* low8, llvm::Value* high8 );
	void InstructionBankWrite8( llvm::Value* address32, llvm::Value* I16, llvm::Value* value );
	void InstructionBankWrite16( llvm::Value* address32, llvm::Value* I16, llvm::Value* low8, llvm::Value* high8 );
	void InstructionLongWrite8( llvm::Value* address32, llvm::Value* I16, llvm::Value* value );
	void InstructionLongWrite16( llvm::Value* address32, llvm::Value* I16, llvm::Value* low8, llvm::Value* high8 );
	void InstructionDirectWrite8( llvm::Value* address32, llvm::Value* value );
	void InstructionDirectWrite16( llvm::Value* address32, llvm::Value* low8, llvm::Value* high8 );
	void InstructionDirectWrite8( llvm::Value* address32, llvm::Value* I16, llvm::Value* value );
	void InstructionDirectWrite16( llvm::Value* address32, llvm::Value* I16, llvm::Value* low8, llvm::Value* high8 );
	void InstructionIndirectWrite8( llvm::Value* address32, llvm::Value* value );
	void InstructionIndirectWrite16( llvm::Value* address32, llvm::Value* low8, llvm::Value* high8 );
	void InstructionIndexedIndirectWrite8( llvm::Value* address32, llvm::Value* value );
	void InstructionIndexedIndirectWrite16( llvm::Value* address32, llvm::Value* low8, llvm::Value* high8 );
	void InstructionIndirectIndexedWrite8( llvm::Value* address32, llvm::Value* value );
	void InstructionIndirectIndexedWrite16( llvm::Value* address32, llvm::Value* low8, llvm::Value* high8 );
	void InstructionIndirectLongWrite8( llvm::Value* address32, llvm::Value* I16, llvm::Value* value );
	void InstructionIndirectLongWrite16( llvm::Value* address32, llvm::Value* I16, llvm::Value* low8, llvm::Value* high8 );
	void InstructionStackWrite8( llvm::Value* address32, llvm::Value* value );
	void InstructionStackWrite16( llvm::Value* address32, llvm::Value* low8, llvm::Value* high8 );
	void InstructionIndirectStackWrite8( llvm::Value* address32, llvm::Value* value );
	void InstructionIndirectStackWrite16( llvm::Value* address32, llvm::Value* low8, llvm::Value* high8 );

	void InstructionBitImmediate8( llvm::Value* operand8 );
	void InstructionBitImmediate16( llvm::Value* operand16 );
	void InstructionTransfer8( llvm::Value* sourceRegisterPtr, llvm::Value* destinationRegisterPtr );
	void InstructionTransfer16( llvm::Value* sourceRegisterPtr, llvm::Value* destinationRegisterPtr );
	void InstructionTransferSX8();
	void InstructionTransferSX16();
	void InstructionPush8( llvm::Value* value8 );
	void InstructionPush16( llvm::Value* low8, llvm::Value* high8 );
	void InstructionPull8( llvm::Value* register16Ptr );
	void InstructionPull16( llvm::Value* register16Ptr );
	void InstructionBlockMove8( llvm::Value* sourceBank32, llvm::Value* destinationBank32, llvm::Value* adjust8, llvm::BasicBlock* blockMove, llvm::BasicBlock* endBlock );
	void InstructionBlockMove16( llvm::Value* sourceBank32, llvm::Value* destinationBank32, llvm::Value* adjust16, llvm::BasicBlock* blockMove, llvm::BasicBlock* endBlock );

	enum class RegisterModeFlag
	{
		REGISTER_MODE_FLAG_M,
		REGISTER_MODE_FLAG_X
	};
	
	void PerformImpliedModifyInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* ptr );
	void PerformBankModifyInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address32 );
	void PerformBankIndexedModifyInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address16 );
	void PerformDirectModifyInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address32 );
	void PerformDirectIndexedModifyInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address16 );

	void PerformImmediateReadInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* operand16 );
	void PerformBankReadInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address32 );
	void PerformBankReadInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address16, llvm::Value* I );
	void PerformLongReadInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address32, llvm::Value* I );
	void PerformDirectReadInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address32 );
	void PerformDirectReadInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address16, llvm::Value* I16 );
	void PerformIndirectReadInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address32 );
	void PerformIndexedIndirectReadInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address32 );
	void PerformIndirectIndexedReadInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address32 );
	void PerformIndirectLongReadInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address32, llvm::Value* I16 );
	void PerformStackReadInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address32 );
	void PerformIndirectStackReadInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address32 );

	void PerformBankWriteInstruction( RegisterModeFlag modeFlag, llvm::Value* address32, llvm::Value* value16 );
	void PerformBankWriteInstruction( RegisterModeFlag modeFlag, llvm::Value* address32, llvm::Value* I16, llvm::Value* value16 );
	void PerformLongWriteInstruction( RegisterModeFlag modeFlag, llvm::Value* address32, llvm::Value* I16 );
	void PerformDirectWriteInstruction( RegisterModeFlag modeFlag, llvm::Value* address32, llvm::Value* value16 );
	void PerformDirectWriteInstruction( RegisterModeFlag modeFlag, llvm::Value* address32, llvm::Value* I16, llvm::Value* value16 );
	void PerformIndirectWriteInstruction( RegisterModeFlag modeFlag, llvm::Value* address32 );
	void PerformIndexedIndirectWriteInstruction( RegisterModeFlag modeFlag, llvm::Value* address32 );
	void PerformIndirectIndexedWriteInstruction( RegisterModeFlag modeFlag, llvm::Value* address32 );
	void PerformIndirectLongWriteInstruction( RegisterModeFlag modeFlag, llvm::Value* address32, llvm::Value* I16 );
	void PerformStackWriteInstruction( RegisterModeFlag modeFlag, llvm::Value* address32 );
	void PerformIndirectStackWriteInstruction( RegisterModeFlag modeFlag, llvm::Value* address32 );

	void PerformBitImmediateInstruction( RegisterModeFlag modeFlag, llvm::Value* operand16 );
	void PerformSetFlagInstruction( llvm::Value* flag );
	void PerformClearFlagInstruction( llvm::Value* flag );
	void PerformExchangeCEInstruction();
	void PerformExchangeBAInstruction();
	void PerformResetPInstruction( llvm::Value* operand8 );
	void PerformSetPInstruction( llvm::Value* operand8 );
	void PerformTransferInstruction( RegisterModeFlag modeFlag, llvm::Value* sourceRegisterPtr, llvm::Value* destinationRegisterPtr );
	void PerformTransfer16Instruction( llvm::Value* sourceRegisterPtr, llvm::Value* destinationRegisterPtr );
	void PerformTransferCSInstruction();
	void PerformTransferSXInstruction( RegisterModeFlag modeFlag );
	void PerformTransferXSInstruction();
	void PerformPushInstruction( RegisterModeFlag modeFlag, llvm::Value* value16 );
	void PerformPush8Instruction( llvm::Value* value8 );
	void PerformPushDInstruction();
	void PerformPullInstruction( RegisterModeFlag modeFlag, llvm::Value* register16Ptr );
	void PerformPullDInstruction();
	void PerformPullBInstruction();
	void PerformPullPInstruction();
	void PerformPushEffectiveAddressInstruction( llvm::Value* operand16 );
	void PerformPushEffectiveIndirectAddressInstruction( llvm::Value* address32 );
	void PerformPushEffectiveRelativeAddressInstruction( llvm::Value* operand16 );
	void PerformBlockMoveInstruction( RegisterModeFlag modeFlag, llvm::Value* operand32, llvm::Value* adjust16 );
	
	void PerformProcessorStatusRegisterForcedConfiguration();
	void PerformStackPointerEmulationFlagForcedConfiguration();

	void PerformBranchInstruction( llvm::Value* cond, const std::string& labelName, const std::string& functionName );
	void PerformJumpInstruction( const std::string& labelName, const std::string& functionName );
	void InsertJumpTable( llvm::Value* switchValue, const uint32_t instructionOffset, const std::string& functionName );
	void PerformJumpIndirectInstruction( const uint32_t instructionOffset, llvm::Value* operand16, const std::string& functionName );
	void PerformJumpIndexedIndirectInstruction( const uint32_t instructionOffset, llvm::Value* operand16, const std::string& functionName );
	void PerformJumpIndirectLongInstruction( const uint32_t instructionOffset, llvm::Value* operand16, const std::string& functionName );

	void InsertFunctionCall( const uint32_t instructionOffset );
	void PerformCallShortInstruction( const uint32_t instructionOffset );
	void PerformCallLongInstruction( const uint32_t instructionOffset );
	void PerformCallIndexedIndirectInstruction( const uint32_t instructionOffset, llvm::Value* operand16 );

	void PerformReturnInterruptInstruction();
	void PerformReturnShortInstruction();
	void PerformReturnLongInstruction();

	llvm::Value* GetPBPC32();

	llvm::Value* GetConstant( uint32_t value, uint32_t bitWidth, bool isSigned );
	llvm::Value* TestBits8( llvm::Value* lhs, uint8_t rhs );
	llvm::Value* TestBits16( llvm::Value* lhs, uint16_t rhs );
	llvm::Value* TestBits32( llvm::Value* lhs, uint32_t rhs );

	llvm::Value* ASL8( llvm::Value* value );
	llvm::Value* ASL16( llvm::Value* value );
	llvm::Value* BIT8( llvm::Value* value );
	llvm::Value* BIT16( llvm::Value* value );
	llvm::Value* DEC8( llvm::Value* value );
	llvm::Value* DEC16( llvm::Value* value );
	llvm::Value* INC8( llvm::Value* value );
	llvm::Value* INC16( llvm::Value* value );
	llvm::Value* ROL8( llvm::Value* value );
	llvm::Value* ROL16( llvm::Value* value );
	llvm::Value* LSR8( llvm::Value* value );
	llvm::Value* LSR16( llvm::Value* value );
	llvm::Value* ROR8( llvm::Value* value );
	llvm::Value* ROR16( llvm::Value* value );
	llvm::Value* TSB8( llvm::Value* value );
	llvm::Value* TSB16( llvm::Value* value );
	llvm::Value* TRB8( llvm::Value* value );
	llvm::Value* TRB16( llvm::Value* value );
	llvm::Value* ORA8( llvm::Value* value );
	llvm::Value* ORA16( llvm::Value* value );
	llvm::Value* AND8( llvm::Value* value );
	llvm::Value* AND16( llvm::Value* value );
	llvm::Value* EOR8( llvm::Value* value );
	llvm::Value* EOR16( llvm::Value* value );
	llvm::Value* LDY8( llvm::Value* value );
	llvm::Value* LDY16( llvm::Value* value );
	llvm::Value* LDX8( llvm::Value* value );
	llvm::Value* LDX16( llvm::Value* value );
	llvm::Value* LDA8( llvm::Value* value );
	llvm::Value* LDA16( llvm::Value* value );
	llvm::Value* CPY8( llvm::Value* value );
	llvm::Value* CPY16( llvm::Value* value );
	llvm::Value* CPX8( llvm::Value* value );
	llvm::Value* CPX16( llvm::Value* value );
	llvm::Value* CMP8( llvm::Value* value );
	llvm::Value* CMP16( llvm::Value* value );
	llvm::Value* ADC8( llvm::Value* value );
	llvm::Value* ADC16( llvm::Value* value );
	llvm::Value* SBC8( llvm::Value* value );
	llvm::Value* SBC16( llvm::Value* value );

	auto CreateRegisterFlagTestBlock( llvm::Value* flagPtr );
	auto CreateCondTestThenElseBlock( llvm::Value* cond );
	auto CreateCondTestThenBlock( llvm::Value* cond );
	void InsertBlockMoveInstructionBlock( llvm::Value* sourceBank32, llvm::Value* destinationBank32 );

	llvm::Value* OrAllValues( llvm::Value* v );

	template<typename... T>
	auto OrAllValues( llvm::Value* s, T... ts );

	llvm::Value* AddAllValues( llvm::Value* v );

	template<typename... T>
	auto AddAllValues( llvm::Value* s, T... ts );

	llvm::Value* LoadFlag8( llvm::Value* flagPtr );
	llvm::Value* GetProcessorStatusRegisterValueFromFlags();
	void SetProcessorStatusFlagsFromValue( llvm::Value* status8 );

	class Label
	{
	public:
		Label( const std::string& name, const uint32_t offset );
		~Label();

		const std::string& GetName() const { return m_Name; }
		uint32_t GetOffset() const { return m_Offset; }

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
		Instruction( const uint32_t offset, const uint32_t pc, const std::string& instructionString, const uint8_t opcode, const uint32_t operand, const uint32_t operand_size, MemoryMode memoryMode, MemoryMode indexMode, const std::set<std::string>& funcNames );
		Instruction( const uint32_t offset, const uint32_t pc, const std::string& instructionString, const uint8_t opcode, const uint32_t operand, const std::string& jumpLabelName, const uint32_t operand_size, MemoryMode memoryMode, MemoryMode indexMode, const std::set<std::string>& funcNames );
		Instruction( const uint32_t offset, const uint32_t pc, const std::string& instructionString, const uint8_t opcode, MemoryMode memoryMode, MemoryMode indexMode, const std::set<std::string>& funcNames );
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
		uint32_t GetPC( void ) const { return m_PC; }
		const std::string& GetJumpLabelName( void ) const { return m_JumpLabelName; }
		const std::set<std::string>& GetFuncNames( void ) const { return m_FuncNames; }

	private:
		uint32_t m_Offset;
		uint32_t m_PC;
		std::string m_InstructionString;
		uint8_t m_Opcode;
		uint32_t m_Operand;
		std::string m_JumpLabelName;
		uint32_t m_OperandSize;
		MemoryMode m_MemoryMode;
		MemoryMode m_IndexMode;
		bool m_HasOperand;
		std::set<std::string> m_FuncNames;
	};

	void GenerateCodeForInstruction( const Instruction& instruction, const std::string& functionName );

	static constexpr uint64_t M_FLAG = 0b00100000u;
	static constexpr uint64_t X_FLAG = 0b00010000u;

	llvm::LLVMContext m_LLVMContext;
	llvm::IRBuilder<> m_IRBuilder;
	llvm::Module m_RecompilationModule;

	std::string m_RomResetFuncName;
	uint32_t m_RomResetAddr;
	std::string m_RomNmiFuncName;
	std::string m_RomIrqFuncName;
	std::set<std::string> m_FunctionNames;
	std::unordered_map< uint32_t, std::unordered_map< std::string, bool > > m_LabelsToFunctions;
	std::unordered_map< uint32_t, std::string > m_OffsetToFunctionName;
	std::unordered_map< uint32_t, std::unordered_map< uint32_t, std::string > > m_JumpTables;
	std::vector< std::variant<Label, Instruction> > m_Program;
	std::unordered_map<std::string, llvm::Function*> m_Functions;
	std::unordered_map< std::string, uint32_t > m_LabelNamesToOffsets;
	std::unordered_map< uint32_t, std::string > m_OffsetsToLabelNames;
	std::unordered_map< std::string, llvm::BasicBlock* > m_LabelNamesToBasicBlocks;
	std::unordered_map< uint32_t, llvm::GlobalVariable* > m_OffsetsToInstructionStringGlobalVariable;
	std::unordered_map< std::string, uint32_t > m_returnAddressManipulationFunctions;
	std::unordered_map< std::string, llvm::BasicBlock* > m_returnAddressManipulationFunctionsBlocks;

	llvm::Function* m_StartFunction;

	llvm::GlobalVariable* m_registerA;
	llvm::GlobalVariable* m_registerDB;
	llvm::GlobalVariable* m_registerDP;
	llvm::GlobalVariable* m_registerPB;
	llvm::GlobalVariable* m_registerPC;
	llvm::GlobalVariable* m_registerSP;
	llvm::GlobalVariable* m_registerX;
	llvm::GlobalVariable* m_registerY;
	llvm::GlobalVariable* m_registerP;

	llvm::GlobalVariable* m_CarryFlag;
	llvm::GlobalVariable* m_ZeroFlag;
	llvm::GlobalVariable* m_InterruptFlag;
	llvm::GlobalVariable* m_DecimalFlag;
	llvm::GlobalVariable* m_IndexRegisterFlag;
	llvm::GlobalVariable* m_AccumulatorFlag;
	llvm::GlobalVariable* m_OverflowFlag;
	llvm::GlobalVariable* m_NegativeFlag;
	llvm::GlobalVariable* m_EmulationFlag;

	llvm::BasicBlock* m_CurrentBasicBlock;
	llvm::Function* m_CycleFunction;
	llvm::Function* m_PanicFunction;
	llvm::Function* m_UpdateInstructionOutput;

	static const uint32_t WAIT_FOR_VBLANK_LOOP_LABEL_OFFSET = 0x805C;
	static inline const std::string WAIT_FOR_VBLANK_LABEL_NAME = "CODE_80805C";

	llvm::Function* m_Load8Function;
	llvm::Function* m_Store8Function;

	llvm::Function* m_DoPPUFrameFunction;

	llvm::Function* m_ADC8Function;
	llvm::Function* m_ADC16Function;
	llvm::Function* m_SBC8Function;
	llvm::Function* m_SBC16Function;
};

#endif // RECOMPILER_HPP