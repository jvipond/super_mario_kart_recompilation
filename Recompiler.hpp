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

	void CreateBranch( llvm::BasicBlock* basicBlock );
	void SetInsertPoint( llvm::BasicBlock* basicBlock );
	void PerformOra( llvm::Value* value );
	void PerformLda( llvm::Value* value );
	void PerformLdx( llvm::Value* value );
	void PerformLdy( llvm::Value* value );
	void PerformCmp( llvm::Value* lValue, llvm::Value* rValue );
	void TestAndSetZero( llvm::Value* value );
	void TestAndSetNegative( llvm::Value* value );
	void TestAndSetCarrySubtraction( llvm::Value* lValue, llvm::Value* rValue );
	void ClearCarry();
	void SetCarry();
	void ClearDecimal();
	void SetDecimal();
	void ClearInterrupt();
	void SetInterrupt();
	void ClearOverflow();



	llvm::Value* CreateLoadA( void );
	llvm::Value* CreateLoadX( void );
	llvm::Value* CreateLoadY( void );

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

	class Instruction
	{
	public:
		Instruction( const uint32_t offset, const uint8_t opcode, const uint32_t operand );
		Instruction( const uint32_t offset, const uint8_t opcode );
		~Instruction();

		uint8_t GetOpcode( void ) const { return m_Opcode; }
		uint32_t GetOperand( void ) const { return m_Operand; }
		bool HasOperand( void ) const { return m_HasOperand; }

	private:
		uint32_t m_Offset;
		uint8_t m_Opcode;
		uint32_t m_Operand;
		bool m_HasOperand;
	};


	llvm::LLVMContext m_LLVMContext;
	llvm::IRBuilder<> m_IRBuilder;
	llvm::Module m_RecompilationModule;

	std::vector< std::variant<Label, Instruction> > m_Program;
	std::map< std::string, uint32_t > m_LabelNamesToOffsets;
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
	llvm::GlobalVariable m_registerStatusBreak;
	llvm::GlobalVariable m_registerStatusCarry;
	llvm::GlobalVariable m_registerStatusDecimal;
	llvm::GlobalVariable m_registerStatusInterrupt;
	llvm::GlobalVariable m_registerStatusMemoryWidth;
	llvm::GlobalVariable m_registerStatusNegative;
	llvm::GlobalVariable m_registerStatusOverflow;
	llvm::GlobalVariable m_registerStatusIndexWidth;
	llvm::GlobalVariable m_registerStatusZero;

	llvm::BasicBlock* m_CurrentBasicBlock;
};

#endif // RECOMPILER_HPP