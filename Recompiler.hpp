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

	private:
		uint32_t m_Offset;
		uint8_t m_Opcode;
		uint32_t m_Operand;
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
};

#endif // RECOMPILER_HPP