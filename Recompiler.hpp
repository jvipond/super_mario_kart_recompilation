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
	void PerformBra( const std::string& labelName );
	void PerformJmp( const std::string& labelName );
	void PerformJsr( const std::string& labelName );
	void PerformJsl( const std::string& labelName );
	void PerformBcc( const std::string& labelName );
	void PerformBcs( const std::string& labelName );
	void PerformBeq( const std::string& labelName );
	void PerformBne( const std::string& labelName );
	void PerformBmi( const std::string& labelName );
	void PerformBpl( const std::string& labelName );
	void PerformBvc( const std::string& labelName );
	void PerformBvs( const std::string& labelName );
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
	void PerformInc16Acc( void );
	void PerformInc8Acc( void );
	void PerformDec( void );
	void PerformDec16Acc( void );
	void PerformDec8Acc( void );
	void PerformInx( void );
	void PerformInx16Acc( void );
	void PerformInx8Acc( void );
	void PerformDex( void );
	void PerformDex16Acc( void );
	void PerformDex8Acc( void );
	void PerformIny( void );
	void PerformIny16Acc( void );
	void PerformIny8Acc( void );
	void PerformDey( void );
	void PerformDey16Acc( void );
	void PerformDey8Acc( void );
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
		Instruction( const uint32_t offset, const uint8_t opcode, const uint32_t operand, const uint32_t operand_size, MemoryMode memoryMode, MemoryMode indexMode );
		Instruction( const uint32_t offset, const uint8_t opcode, const uint32_t operand, const std::string& jumpLabelName, const uint32_t operand_size, MemoryMode memoryMode, MemoryMode indexMode );
		Instruction( const uint32_t offset, const uint8_t opcode, MemoryMode memoryMode, MemoryMode indexMode );
		~Instruction();

		uint8_t GetOpcode( void ) const { return m_Opcode; }
		uint32_t GetOperand( void ) const { return m_Operand; }
		uint32_t GetOperandSize( void ) const { return m_OperandSize; }
		bool HasOperand( void ) const { return m_HasOperand; }
		const MemoryMode& GetMemoryMode() const { return m_MemoryMode; }
		const MemoryMode& GetIndexMode() const { return m_IndexMode; }
		uint32_t GetOffset( void ) const { return m_Offset; }
		const std::string& GetJumpLabelName( void ) const { return m_JumpLabelName; }

	private:
		uint32_t m_Offset;
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
	std::string m_RomNmiLabelName;
	std::string m_RomIrqLabelName;
	std::vector< std::variant<Label, Instruction> > m_Program;
	std::map< std::string, uint32_t > m_LabelNamesToOffsets;
	std::map< uint32_t, std::string > m_OffsetsToLabelNames;
	std::map< std::string, llvm::BasicBlock* > m_LabelNamesToBasicBlocks;
	std::map< uint32_t, llvm::BasicBlock* > m_DynamicJumpOffsetsToBasicBlocks;

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
	llvm::GlobalVariable m_registerStatusBreak;
	llvm::GlobalVariable m_registerStatusCarry;
	llvm::GlobalVariable m_registerStatusDecimal;
	llvm::GlobalVariable m_registerStatusInterrupt;
	llvm::GlobalVariable m_registerStatusMemoryWidth;
	llvm::GlobalVariable m_registerStatusNegative;
	llvm::GlobalVariable m_registerStatusOverflow;
	llvm::GlobalVariable m_registerStatusIndexWidth;
	llvm::GlobalVariable m_registerStatusZero;

	static const uint32_t WRAM_SIZE = 0x20000;
	llvm::GlobalVariable m_wRam;

	llvm::BasicBlock* m_CurrentBasicBlock;
};

#endif // RECOMPILER_HPP