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

	void LoadAST( const char* filename );
	void Recompile();

	void AddLabelNameToBasicBlock( const std::string& labelName, llvm::BasicBlock* basicBlock );
	void CreateFunctions();
	void InitialiseBasicBlocksFromLabelNames();
	void GenerateCode();
	void EnforceFunctionEntryBlocksConstraints();
	void SetupNmiCall();
	void AddOffsetToInstructionString( const uint32_t offset, const std::string& stringGlobalVariable );
	void AddInstructionStringGlobalVariables();
	void SelectBlock( llvm::BasicBlock* basicBlock );

	const std::unordered_map<uint32_t, std::unordered_map<std::string, bool> >& GetLabelsToFunctions() const { return m_LabelsToFunctions; }
	const std::unordered_map<std::string, llvm::Function*>& GetFunctions() const { return m_Functions; }

	llvm::BasicBlock* CreateBlock( const char* name );
	void CreateBranch( llvm::BasicBlock* basicBlock );
	void SetInsertPoint( llvm::BasicBlock* basicBlock );
	auto CreateRegisterWidthTest( const uint64_t flag );
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
	void PerformAdcAbs( const uint32_t address );
	void PerformAdcAbsIdxX( const uint32_t address );
	void PerformAdcDir( const uint32_t address );
	void PerformAdcLong( const uint32_t address );
	void PerformSbcAbs( const uint32_t address );
	void PerformSbcAbsIdxX( const uint32_t address );
	void PerformSbcDir( const uint32_t address );
	void PerformSbcDirIdxX( const uint32_t address );
	void PerformAdcLongIdxX( const uint32_t address );
	void PerformSbcLongIdxX( const uint32_t address );
	void PerformSbc16( llvm::Value* value );
	void PerformSbc8( llvm::Value* value );
	void PerformBit16Imm( llvm::Value* value );
	void PerformBit8Imm( llvm::Value* value );
	void TestAndSetZero16( llvm::Value* value );
	void TestAndSetZero8( llvm::Value* value );
	void TestAndSetNegative16( llvm::Value* value );
	void TestAndSetNegative8( llvm::Value* value );
	void TestAndSetCarryAddition16( llvm::Value* value );
	void TestAndSetCarryAddition8( llvm::Value* value );
	void TestAndSetOverflowAddition16( llvm::Value* value );
	void TestAndSetOverflowAddition8( llvm::Value* value );
	void TestAndSetCarrySubtraction16( llvm::Value* value );
	void TestAndSetCarrySubtraction8( llvm::Value* value );
	void TestAndSetOverflow16( llvm::Value* value );
	void TestAndSetOverflow8( llvm::Value* value );
	void PerformXba();
	void PerformTcs();
	void PerformTcd();
	void PerformTdc();
	void PerformTsc();
	void PerformRtl();
	void PerformRts();
	void PerformRti();
	void PerformPea( llvm::Value* value );
	void PerformBra( const std::string& labelName, const std::string& functionName, llvm::Value* newPC );
	void PerformJmpAbs( const std::string& labelName, const std::string& functionName, const uint32_t jumpAddress );
	void PerformJmpLng( const std::string& labelName, const std::string& functionName, const uint32_t jumpAddress );
	void PerformJsr( const uint32_t instructionOffset, const uint32_t jumpAddress );
	void PerformJsrAbsIdxX( const uint32_t instructionOffset, const uint32_t jumpAddress );
	void PerformJmpAbsIdxX( const uint32_t instructionOffset, const std::string& functionName, const uint32_t jumpAddress );
	void PerformJsl( const uint32_t instructionOffset, const uint32_t jumpAddress );
	void PerformBcc( const std::string& labelName, const std::string& functionName, llvm::Value* pcBranchTaken, llvm::Value* pcBranchNotTaken );
	void PerformBcs( const std::string& labelName, const std::string& functionName, llvm::Value* pcBranchTaken, llvm::Value* pcBranchNotTaken );
	void PerformBeq( const std::string& labelName, const std::string& functionName, llvm::Value* pcBranchTaken, llvm::Value* pcBranchNotTaken );
	void PerformBne( const std::string& labelName, const std::string& functionName, llvm::Value* pcBranchTaken, llvm::Value* pcBranchNotTaken );
	void PerformBmi( const std::string& labelName, const std::string& functionName, llvm::Value* pcBranchTaken, llvm::Value* pcBranchNotTaken );
	void PerformBpl( const std::string& labelName, const std::string& functionName, llvm::Value* pcBranchTaken, llvm::Value* pcBranchNotTaken );
	void PerformBvc( const std::string& labelName, const std::string& functionName, llvm::Value* pcBranchTaken, llvm::Value* pcBranchNotTaken );
	void PerformBvs( const std::string& labelName, const std::string& functionName, llvm::Value* pcBranchTaken, llvm::Value* pcBranchNotTaken );
	void AddConditionalBranch( llvm::Value* cond, const std::string& labelName, const std::string& functionName, llvm::Value* pcBranchTaken, llvm::Value* pcBranchNotTaken );
	void PerformSep( llvm::Value* value );
	void PerformRep( llvm::Value* value );
	void PerformPha( void );
	void PerformPla( void );
	void PerformPhy( void );
	void PerformPly( void );
	void PerformPhx( void );
	void PerformPlx( void );
	void PerformPhb( void );
	void PerformPhd( void );
	void PerformPhk( void );
	void PerformPhp( void );
	void PerformPlb( void );
	void PerformPld( void );
	void PerformPlp( void );
	void PerformInc( void );
	void PerformInc16( llvm::Value* ptr );
	void PerformInc8( llvm::Value* ptr );
	void PerformInc16( llvm::Value* address, llvm::Value* value );
	void PerformInc8( llvm::Value* address, llvm::Value* value );
	void PerformIncAbs( const uint32_t address );
	void PerformIncDir( const uint32_t address );
	void PerformDec( void );
	void PerformDecAbs( const uint32_t address );
	void PerformDecDir( const uint32_t address );
	void PerformDec16( llvm::Value* ptr );
	void PerformDec8( llvm::Value* ptr );
	void PerformDec16( llvm::Value* address, llvm::Value* value );
	void PerformDec8( llvm::Value* address, llvm::Value* value );
	void PerformInx( void );
	void PerformDex( void );
	void PerformIny( void );
	void PerformDey( void );
	void PerformAsl( void );
	void PerformAslAbs( const uint32_t address );
	void PerformAslDir( const uint32_t address );
	void PerformAsl16( llvm::Value* ptr );
	void PerformAsl8( llvm::Value* ptr );
	void PerformAsl16( llvm::Value* address, llvm::Value* value );
	void PerformAsl8( llvm::Value* address, llvm::Value* value );
	void PerformLsr( void );
	void PerformLsrAbs( const uint32_t address );
	void PerformLsrDir( const uint32_t address );
	void PerformLsr16( llvm::Value* ptr );
	void PerformLsr8( llvm::Value* ptr );
	void PerformLsr16( llvm::Value* address, llvm::Value* value );
	void PerformLsr8( llvm::Value* address, llvm::Value* value );
	void PerformRol( void );
	void PerformRolAbs( const uint32_t address );
	void PerformRolDir( const uint32_t address );
	void PerformRol16( llvm::Value* ptr );
	void PerformRol8( llvm::Value* ptr );
	void PerformRol16( llvm::Value* address, llvm::Value* value );
	void PerformRol8( llvm::Value* address, llvm::Value* value );
	void PerformRor( void );
	void PerformRorAbs( const uint32_t address );
	void PerformRorDir( const uint32_t address );
	void PerformRor16( llvm::Value* ptr );
	void PerformRor8( llvm::Value* ptr );
	void PerformRor16( llvm::Value* address, llvm::Value* value );
	void PerformRor8( llvm::Value* address, llvm::Value* value );
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
	void PerformRomCycle( llvm::Value* value, const bool implemented = true );
	void PerformRomCycle( llvm::Value* value, llvm::Value* newPc, const bool implemented = true );
	void PerformUpdateInstructionOutput( const uint32_t offset, const std::string& instructionString );
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
	llvm::Value* wRamPtr8( const uint32_t offset );
	llvm::Value* romPtr8( const uint32_t offset );
	llvm::Value* wRamPtr8( llvm::Value* offset );
	llvm::Value* romPtr8( llvm::Value* offset );
	void PerformLdaLong( const uint32_t address );
	void PerformLdaAbs( const uint32_t address );
	void PerformLdaAbsIdxX( const uint32_t address );
	void PerformLdaAbsIdxY( const uint32_t address );
	void PerformLdaDir( const uint32_t address );
	void PerformLdaDirIdxX( const uint32_t address );
	void PerformLdaLongIdxX( const uint32_t address );
	void PerformLdaDirIndLngIdxY( const uint32_t address );
	void PerformLdxAbs( const uint32_t address );
	void PerformLdxAbsIdxY( const uint32_t address );
	void PerformLdxDir( const uint32_t address );
	void PerformLdxDirIdxY( const uint32_t address );
	void PerformLdyAbs( const uint32_t address );
	void PerformLdyAbsIdxX( const uint32_t address );
	void PerformLdyDir( const uint32_t address );
	void PerformLdyDirIdxX( const uint32_t address );
	void PerformStaLong( const uint32_t address );
	void PerformStaLongIdxX( const uint32_t address );
	void PerformStaAbs( const uint32_t address );
	void PerformStaAbsIdxX( const uint32_t address );
	void PerformStaAbsIdxY( const uint32_t address );
	void PerformStxAbs( const uint32_t address );
	void PerformStyAbs( const uint32_t address );
	void PerformStzAbs( const uint32_t address );
	void PerformStzAbsIdxX( const uint32_t address );
	void PerformStaDir( const uint32_t address );
	void PerformStaDirIdxX( const uint32_t address );
	void PerformStxDir( const uint32_t address );
	void PerformStxDirIdxY( const uint32_t address );
	void PerformStyDir( const uint32_t address );
	void PerformStyDirIdxX( const uint32_t address );
	void PerformStzDir( const uint32_t address );
	void PerformStzDirIdxX( const uint32_t address );
	void PerformMvn( const uint32_t operand, const uint32_t instructionOffset, const std::string& instructionString );
	void PerformMvn16( const uint32_t operand, const uint32_t instructionOffset, const std::string& instructionString );
	void PerformMvn8( const uint32_t operand, const uint32_t instructionOffset, const std::string& instructionString );
	void PerformMvp( const uint32_t operand, const uint32_t instructionOffset, const std::string& instructionString );
	void PerformMvp16( const uint32_t operand, const uint32_t instructionOffset, const std::string& instructionString );
	void PerformMvp8( const uint32_t operand, const uint32_t instructionOffset, const std::string& instructionString );
	void PerformTsbAbs( const uint32_t address );
	void PerformTsbDir( const uint32_t address );
	void PerformTrbAbs( const uint32_t address );
	void PerformTrbDir( const uint32_t address );
	void PerformTsb16( llvm::Value* address, llvm::Value* value );
	void PerformTsb8( llvm::Value* address, llvm::Value* value );
	void PerformTrb16( llvm::Value* address, llvm::Value* value );
	void PerformTrb8( llvm::Value* address, llvm::Value* value );
	void PerformBitAbs( const uint32_t address );
	void PerformBitDir( const uint32_t address );
	void PerformBit16( llvm::Value* value );
	void PerformBit8( llvm::Value* value );
	void PerformAndAbs( const uint32_t address );
	void PerformAndDir( const uint32_t address );
	void PerformEorAbs( const uint32_t address );
	void PerformEorDir( const uint32_t address );
	void PerformEorDirIdxX( const uint32_t address );
	void PerformOraAbs( const uint32_t address );
	void PerformOraAbsIdxY( const uint32_t address );
	void PerformOraDir( const uint32_t address );
	void PerformCmpAbs( const uint32_t address );
	void PerformCmpDir( const uint32_t address );
	void PerformCpxAbs( const uint32_t address );
	void PerformCpxDir( const uint32_t address );
	void PerformCpyAbs( const uint32_t address );
	void PerformCpyDir( const uint32_t address );

private:
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
		Instruction( const uint32_t offset, const std::string& instructionString, const uint8_t opcode, const uint32_t operand, const uint32_t operand_size, MemoryMode memoryMode, MemoryMode indexMode, const std::set<std::string>& funcNames );
		Instruction( const uint32_t offset, const std::string& instructionString, const uint8_t opcode, const uint32_t operand, const std::string& jumpLabelName, const uint32_t operand_size, MemoryMode memoryMode, MemoryMode indexMode, const std::set<std::string>& funcNames );
		Instruction( const uint32_t offset, const std::string& instructionString, const uint8_t opcode, MemoryMode memoryMode, MemoryMode indexMode, const std::set<std::string>& funcNames );
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
		const std::set<std::string>& GetFuncNames( void ) const { return m_FuncNames; }

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
	llvm::Function* m_UpdateInstructionOutput;

	static const uint32_t WAIT_FOR_VBLANK_LOOP_LABEL_OFFSET = 0x805C;
	static inline const std::string WAIT_FOR_VBLANK_LABEL_NAME = "CODE_80805C";

	llvm::Function* m_Load8Function;
	llvm::Function* m_Load16Function;
	llvm::Function* m_Store8Function;
	llvm::Function* m_Store16Function;
};

#endif // RECOMPILER_HPP