#include "Recompiler.hpp"
#include "json.hpp"
#include <fstream>
#include <iostream>
#include <iomanip>
#include "llvm/Support/TargetSelect.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Type.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Verifier.h"

Recompiler::Recompiler()
: m_IRBuilder( m_LLVMContext )
, m_RecompilationModule( "recompilation", m_LLVMContext )
, m_DynamicJumpTableBlock( nullptr )
, m_MainFunction( nullptr )
, m_registerA( m_RecompilationModule, llvm::Type::getInt16Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "A" )
, m_registerDB( m_RecompilationModule, llvm::Type::getInt8Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "DB" )
, m_registerDP( m_RecompilationModule, llvm::Type::getInt16Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "DP" )
, m_registerPB( m_RecompilationModule, llvm::Type::getInt8Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "PB" )
, m_registerPC( m_RecompilationModule, llvm::Type::getInt16Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "PC" )
, m_registerSP( m_RecompilationModule, llvm::Type::getInt16Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "SP" )
, m_registerX( m_RecompilationModule, llvm::Type::getInt16Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "X" )
, m_registerY( m_RecompilationModule, llvm::Type::getInt16Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "Y" )
, m_registerP( m_RecompilationModule, llvm::Type::getInt8Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "P" )
, m_wRam( m_RecompilationModule, llvm::ArrayType::get( llvm::Type::getInt8Ty( m_LLVMContext ), WRAM_SIZE ), false, llvm::GlobalValue::ExternalLinkage, 0, "wRam" )
, m_CurrentBasicBlock( nullptr )
, m_CycleFunction( nullptr )
, m_PanicFunction( nullptr )
, m_PanicBlock( nullptr )
{
}

Recompiler::~Recompiler()
{
	m_registerA.removeFromParent();
	m_registerDB.removeFromParent();
	m_registerDP.removeFromParent();
	m_registerPB.removeFromParent();
	m_registerPC.removeFromParent();
	m_registerSP.removeFromParent();
	m_registerX.removeFromParent();
	m_registerY.removeFromParent();
	m_registerP.removeFromParent();
	m_wRam.removeFromParent();
}

// Visit all the Label nodes and set up the basic blocks:
void Recompiler::InitialiseBasicBlocksFromLabelNames()
{
	struct InitialiseBasicBlocksFromLabelsVisitor
	{
		InitialiseBasicBlocksFromLabelsVisitor( Recompiler& recompiler, llvm::Function* function, llvm::LLVMContext& context ) : m_Recompiler( recompiler ), m_Function( function ), m_Context( context ) {}
		
		void operator()( const Label& label )
		{
			const std::string& labelName = label.GetName();
			llvm::BasicBlock* basicBlock = llvm::BasicBlock::Create( m_Context, labelName, m_Function );
			m_Recompiler.AddLabelNameToBasicBlock( labelName, basicBlock );
		}

		void operator()( const Instruction& )
		{
		}

		Recompiler& m_Recompiler;
		llvm::Function* m_Function;
		llvm::LLVMContext& m_Context;
	};

	InitialiseBasicBlocksFromLabelsVisitor initialiseBasicBlocksFromLabelsVisitor( *this, m_MainFunction, m_LLVMContext );
	for ( auto&& n : m_Program )
	{
		std::visit( initialiseBasicBlocksFromLabelsVisitor, n );
	}
}

void Recompiler::AddDynamicJumpTableBlock()
{
	// add dynamic jump table block:
	llvm::BasicBlock* dynamicJumpTableDefaultCaseBlock = llvm::BasicBlock::Create( m_LLVMContext, "DynamicJumpTableDefaultCaseBlock", m_MainFunction );
	m_DynamicJumpTableBlock = llvm::BasicBlock::Create( m_LLVMContext, "DynamicJumpTable", m_MainFunction );
	m_IRBuilder.SetInsertPoint( m_DynamicJumpTableBlock );

	auto pc32 = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerPC, "" ), llvm::Type::getInt32Ty( m_LLVMContext ), "" );
	auto pb32 = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerPB, "" ), llvm::Type::getInt32Ty( m_LLVMContext ), "" );
	auto pb32Shifted = m_IRBuilder.CreateShl( pb32, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 24 ), false ) ), "" );
	auto finalPC = m_IRBuilder.CreateOr( pc32, pb32Shifted, "" );
	auto sw = m_IRBuilder.CreateSwitch( finalPC, dynamicJumpTableDefaultCaseBlock, static_cast<unsigned int>( m_LabelNamesToOffsets.size() ) );
	SelectBlock( dynamicJumpTableDefaultCaseBlock );
	m_IRBuilder.CreateRetVoid();
	for ( auto&& entry : m_LabelNamesToOffsets )
	{
		auto addressValue = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( entry.second ), false ) );
		if ( sw )
		{
			sw->addCase( addressValue, m_LabelNamesToBasicBlocks[ entry.first ] );
		}
	}
	SelectBlock( nullptr );
}

void Recompiler::GenerateCode()
{
	// Visit all program nodes for codegen:
	struct CodeGenerationVisitor
	{
		CodeGenerationVisitor( Recompiler& recompiler, llvm::Function* function, llvm::LLVMContext& context ) : m_Recompiler( recompiler ), m_Function( function ), m_Context( context ) {}
		
		void operator()( const Label& label )
		{
			auto labelNamesToBasicBlocks = m_Recompiler.GetLabelNamesToBasicBlocks();
			auto search = labelNamesToBasicBlocks.find( label.GetName() );
			assert( search != labelNamesToBasicBlocks.end() );
			
			if ( m_Recompiler.GetCurrentBasicBlock() != nullptr )
			{
				m_Recompiler.CreateBranch( search->second );
			}

			m_Recompiler.SetCurrentBasicBlock( search->second );
			m_Recompiler.SetInsertPoint( search->second );
		}

		void operator()( const Instruction& instruction )
		{
			switch ( instruction.GetOpcode() )
			{
			case 0x00:
				break;
			case 0x01:
				break;
			case 0x02:
				break;
			case 0x03:
				break;
			case 0x04:
				break;
			case 0x05:
				break;
			case 0x06:
				break;
			case 0x07:
				break;
			case 0x09: // ORA immediate
			{
				if ( instruction.GetMemoryMode() == SIXTEEN_BIT )
				{
					llvm::Value* operandValue = llvm::ConstantInt::get( m_Context, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
					m_Recompiler.PerformOra16( operandValue );
				}
				else
				{
					llvm::Value* operandValue = llvm::ConstantInt::get( m_Context, llvm::APInt( 8, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
					m_Recompiler.PerformOra8( operandValue );
				}
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0x29: // AND immediate
			{
				if ( instruction.GetMemoryMode() == SIXTEEN_BIT )
				{
					llvm::Value* operandValue = llvm::ConstantInt::get( m_Context, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
					m_Recompiler.PerformAnd16( operandValue );
				}
				else
				{
					llvm::Value* operandValue = llvm::ConstantInt::get( m_Context, llvm::APInt( 8, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
					m_Recompiler.PerformAnd8( operandValue );
				}
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0x49: // EOR immediate
			{
				if ( instruction.GetMemoryMode() == SIXTEEN_BIT )
				{
					llvm::Value* operandValue = llvm::ConstantInt::get( m_Context, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
					m_Recompiler.PerformEor16( operandValue );
				}
				else
				{
					llvm::Value* operandValue = llvm::ConstantInt::get( m_Context, llvm::APInt( 8, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
					m_Recompiler.PerformEor8( operandValue );
				}
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0xA9: // LDA immediate
			{
				if ( instruction.GetMemoryMode() == SIXTEEN_BIT )
				{
					llvm::Value* operandValue = llvm::ConstantInt::get( m_Context, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
					m_Recompiler.PerformLda16( operandValue );
				}
				else
				{
					llvm::Value* operandValue = llvm::ConstantInt::get( m_Context, llvm::APInt( 8, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
					m_Recompiler.PerformLda8( operandValue );
				}
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0xA2: // LDX immediate
			{
				if ( instruction.GetIndexMode() == SIXTEEN_BIT )
				{
					llvm::Value* operandValue = llvm::ConstantInt::get( m_Context, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
					m_Recompiler.PerformLdx16( operandValue );
				}
				else
				{
					llvm::Value* operandValue = llvm::ConstantInt::get( m_Context, llvm::APInt( 8, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
					m_Recompiler.PerformLdx8( operandValue );
				}
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0xA0: // LDY immediate
			{
				if ( instruction.GetIndexMode() == SIXTEEN_BIT )
				{
					llvm::Value* operandValue = llvm::ConstantInt::get( m_Context, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
					m_Recompiler.PerformLdy16( operandValue );
				}
				else
				{
					llvm::Value* operandValue = llvm::ConstantInt::get( m_Context, llvm::APInt( 8, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
					m_Recompiler.PerformLdy8( operandValue );
				}
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0xC9: // CMP immediate
			{
				if ( instruction.GetMemoryMode() == SIXTEEN_BIT )
				{
					llvm::Value* lValue = m_Recompiler.CreateLoadA16();
					llvm::Value* rValue = llvm::ConstantInt::get( m_Context, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
					m_Recompiler.PerformCmp16( lValue, rValue );
				}
				else
				{
					llvm::Value* lValue = m_Recompiler.CreateLoadA8();
					llvm::Value* rValue = llvm::ConstantInt::get( m_Context, llvm::APInt( 8, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
					m_Recompiler.PerformCmp8( lValue, rValue );
				}
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0xE0: // CPX immediate
			{
				if ( instruction.GetIndexMode() == SIXTEEN_BIT )
				{
					llvm::Value* lValue = m_Recompiler.CreateLoadX16();
					llvm::Value* rValue = llvm::ConstantInt::get( m_Context, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
					m_Recompiler.PerformCmp16( lValue, rValue );
				}
				else
				{
					llvm::Value* lValue = m_Recompiler.CreateLoadX8();
					llvm::Value* rValue = llvm::ConstantInt::get( m_Context, llvm::APInt( 8, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
					m_Recompiler.PerformCmp8( lValue, rValue );
				}
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0xC0: // CPY immediate
			{
				if ( instruction.GetIndexMode() == SIXTEEN_BIT )
				{
					llvm::Value* lValue = m_Recompiler.CreateLoadY16();
					llvm::Value* rValue = llvm::ConstantInt::get( m_Context, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
					m_Recompiler.PerformCmp16( lValue, rValue );
				}
				else
				{
					llvm::Value* lValue = m_Recompiler.CreateLoadY8();
					llvm::Value* rValue = llvm::ConstantInt::get( m_Context, llvm::APInt( 8, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
					m_Recompiler.PerformCmp8( lValue, rValue );
				}
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0x18: // CLC implied
			{
				m_Recompiler.ClearCarry();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0x38: // SEC implied
			{
				m_Recompiler.SetCarry();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0xD8: // CLD implied
			{
				m_Recompiler.ClearDecimal();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0x58: // CLI implied
			{
				m_Recompiler.ClearInterrupt();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0xB8: // CLV implied
			{
				m_Recompiler.ClearOverflow();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0xF8: // SED implied
			{
				m_Recompiler.SetDecimal();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0x78: // SEI implied
			{
				m_Recompiler.SetInterrupt();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0xEB: // XBA
			{
				m_Recompiler.PerformXba();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0x1B: // TCS
			{
				m_Recompiler.PerformTcs();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0x5B: // TCD
			{
				m_Recompiler.PerformTcd();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0x7B: // TDC
			{
				m_Recompiler.PerformTdc();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0x3B: // TSC
			{
				m_Recompiler.PerformTsc();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0x60: // RTS
			{
				m_Recompiler.PerformRts();
			}
			break;
			case 0x6B: // RTL
			{
				m_Recompiler.PerformRtl();
			}
			break;
			case 0x40: // RTI
			{
				m_Recompiler.PerformRti();
			}
			break;
			case 0x80: // BRA
			{
				m_Recompiler.PerformBra( instruction.GetJumpLabelName() );
			}
			break;
			case 0x4C: // JMP abs
			case 0x5C: // JMP long
			{		
				m_Recompiler.PerformJmp( instruction.GetJumpLabelName() );
			}
			break;
			case 0xF4: // PEA
			{
				llvm::Value* value = llvm::ConstantInt::get( m_Context, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), false ) );
				m_Recompiler.PerformPea( value );
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0xC2: // REP
			{
				llvm::Value* value = llvm::ConstantInt::get( m_Context, llvm::APInt( 8, static_cast<uint64_t>( instruction.GetOperand() ), false ) );
				m_Recompiler.PerformRep( value );
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0xE2: // SEP
			{
				llvm::Value* value = llvm::ConstantInt::get( m_Context, llvm::APInt( 8, static_cast<uint64_t>( instruction.GetOperand() ), false ) );
				m_Recompiler.PerformSep( value );
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0x20: // JSR absolute
			{
				m_Recompiler.PerformJsr( instruction.GetJumpLabelName() );
			}
			break;
			case 0x22: // JSL long
			{
				m_Recompiler.PerformJsl( instruction.GetJumpLabelName() );
			}
			break;
			case 0x90: // BCC
			{
				m_Recompiler.PerformBcc( instruction.GetJumpLabelName() );
			}
			break;
			case 0xB0: // BCS
			{
				m_Recompiler.PerformBcs( instruction.GetJumpLabelName() );
			}
			break;
			case 0xF0: // BEQ
			{
				m_Recompiler.PerformBeq( instruction.GetJumpLabelName() );
			}
			break;
			case 0xD0: // BNE
			{
				m_Recompiler.PerformBne( instruction.GetJumpLabelName() );
			}
			break;
			case 0x30: // BMI
			{
				m_Recompiler.PerformBmi( instruction.GetJumpLabelName() );
			}
			break;
			case 0x10: // BPL
			{
				m_Recompiler.PerformBpl( instruction.GetJumpLabelName() );
			}
			break;
			case 0x50: // BVC
			{
				m_Recompiler.PerformBvc( instruction.GetJumpLabelName() );
			}
			break;
			case 0x70: // BVS
			{
				m_Recompiler.PerformBvs( instruction.GetJumpLabelName() );
			}
			break;
			case 0x8B: // PHB
			{
				m_Recompiler.PerformPhb();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0x0B: // PHD
			{
				m_Recompiler.PerformPhd();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0x4B: // PHK
			{
				m_Recompiler.PerformPhk();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0x08: // PHP
			{
				m_Recompiler.PerformPhp();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0xAB: // PLB
			{
				m_Recompiler.PerformPlb();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0x2B: // PLD
			{
				m_Recompiler.PerformPld();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0x28: // PLP
			{
				m_Recompiler.PerformPlp();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0x3A: // DEC accumulator
			{
				m_Recompiler.PerformDec();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0x1A: // INC accumulator
			{
				m_Recompiler.PerformInc();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0xCA: // DEX accumulator
			{
				m_Recompiler.PerformDex();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0xE8: // INX accumulator
			{
				m_Recompiler.PerformInx();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0x88: // DEY accumulator
			{
				m_Recompiler.PerformDey();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0xC8: // INY accumulator
			{
				m_Recompiler.PerformIny();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0x0A: // ASL accumulator
			{
				m_Recompiler.PerformAsl();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0x4A: // LSR accumulator
			{
				m_Recompiler.PerformLsr();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0x2A: // ROL accumulator
			{
				m_Recompiler.PerformRol();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			case 0x6A: // ROR accumulator
			{
				m_Recompiler.PerformRor();
				m_Recompiler.PerformRomCycle( llvm::ConstantInt::get( m_Context, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
			}
			break;
			}
		}

		Recompiler& m_Recompiler;
		llvm::Function* m_Function;
		llvm::LLVMContext& m_Context;
	};

	CodeGenerationVisitor codeGenerationVisitor( *this, m_MainFunction, m_LLVMContext );
	for ( auto&& n : m_Program )
	{
		std::visit( codeGenerationVisitor, n );
	}
}

void Recompiler::Recompile()
{
	llvm::InitializeNativeTarget();

	// Add cycle function that will called every time an instruction is executed:
	m_CycleFunction = llvm::Function::Create( llvm::FunctionType::get( llvm::Type::getVoidTy( m_LLVMContext ), llvm::Type::getInt32Ty( m_LLVMContext ), false ), llvm::Function::ExternalLinkage, "romCycle", m_RecompilationModule );
	m_PanicFunction = llvm::Function::Create( llvm::FunctionType::get( llvm::Type::getVoidTy( m_LLVMContext ), false ), llvm::Function::ExternalLinkage, "panic", m_RecompilationModule );

	llvm::FunctionType* mainFunctionType = llvm::FunctionType::get( llvm::Type::getVoidTy( m_LLVMContext ), llvm::Type::getInt32Ty( m_LLVMContext ), false );
	m_MainFunction = llvm::Function::Create( mainFunctionType, llvm::Function::ExternalLinkage, "start", m_RecompilationModule );

	llvm::BasicBlock* entry = llvm::BasicBlock::Create( m_LLVMContext, "EntryBlock", m_MainFunction );
	m_PanicBlock = llvm::BasicBlock::Create( m_LLVMContext, "PanicBlock", m_MainFunction );
	m_IRBuilder.SetInsertPoint( m_PanicBlock );
	m_IRBuilder.CreateCall( m_PanicFunction );
	m_IRBuilder.CreateRetVoid();
	m_IRBuilder.SetInsertPoint( entry );

	InitialiseBasicBlocksFromLabelNames();
	AddDynamicJumpTableBlock();
	GenerateCode();

	SelectBlock( entry );
	auto badInterruptBlock = llvm::BasicBlock::Create( m_LLVMContext, "BadInterruptBlock", m_MainFunction );
	auto sw = m_IRBuilder.CreateSwitch( m_MainFunction->arg_begin(), badInterruptBlock, 3 );
	SelectBlock( badInterruptBlock );
	m_IRBuilder.CreateRetVoid();
	sw->addCase( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 1, true ) ), m_LabelNamesToBasicBlocks[ m_RomResetLabelName ] );
	sw->addCase( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 2, true ) ), m_LabelNamesToBasicBlocks[ m_RomNmiLabelName ] );
	sw->addCase( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 3, true ) ), m_LabelNamesToBasicBlocks[ m_RomIrqLabelName ] );

	AddEnterNmiInterruptCode();

	llvm::verifyModule( m_RecompilationModule, &llvm::errs() );

	std::error_code EC;
	llvm::raw_fd_ostream outputHumanReadable( "smk.ll", EC );
	m_RecompilationModule.print( outputHumanReadable, nullptr );
}

void Recompiler::AddEnterNmiInterruptCode( void )
{
	m_IRBuilder.SetInsertPoint( &( *m_LabelNamesToBasicBlocks[ m_RomNmiLabelName ]->getFirstInsertionPt() ) );
	PushByteToStack( m_IRBuilder.CreateLoad( &m_registerP, "" ) );
	PushWordToStack( m_IRBuilder.CreateLoad( &m_registerPC, "" ) );
	PushByteToStack( m_IRBuilder.CreateLoad( &m_registerPB, "" ) );
}

llvm::BasicBlock* Recompiler::CreateIf( llvm::Value* cond )
{
	llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create( m_LLVMContext, "else" );
	elseBlock->moveAfter( m_CurrentBasicBlock );
	llvm::BasicBlock* thenBlock = llvm::BasicBlock::Create( m_LLVMContext, "then" );
	thenBlock->moveAfter( m_CurrentBasicBlock );
	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	return elseBlock;
}

void Recompiler::AddLabelNameToBasicBlock( const std::string& labelName, llvm::BasicBlock* basicBlock )
{
	m_LabelNamesToBasicBlocks.emplace( labelName, basicBlock );
}

llvm::BasicBlock* Recompiler::CreateBlock( const char* name )
{
	llvm::BasicBlock* block = llvm::BasicBlock::Create( m_LLVMContext, name );
	block->moveAfter( m_CurrentBasicBlock );
	return block;
}

void Recompiler::CreateBranch( llvm::BasicBlock* basicBlock )
{
	m_IRBuilder.CreateBr( basicBlock );
}

void Recompiler::SelectBlock( llvm::BasicBlock* basicBlock )
{
	m_IRBuilder.SetInsertPoint( basicBlock );
	m_CurrentBasicBlock = basicBlock;
}

void Recompiler::SetInsertPoint( llvm::BasicBlock* basicBlock )
{
	m_IRBuilder.SetInsertPoint( basicBlock );
}

void Recompiler::Panic( void )
{
	m_IRBuilder.CreateCall( m_CycleFunction );
}

void Recompiler::PerformRomCycle( llvm::Value* value )
{
	std::vector<llvm::Value*> params = { value };
	m_IRBuilder.CreateCall( m_CycleFunction, params, "" );
}

void Recompiler::PerformSep( llvm::Value* value )
{
	auto result = m_IRBuilder.CreateOr( m_IRBuilder.CreateLoad( &m_registerP, "" ), value, "" );
	m_IRBuilder.CreateStore( result, &m_registerP );
}

void Recompiler::PerformRep( llvm::Value* value )
{
	auto complement = m_IRBuilder.CreateXor( value, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0xff, true ) ), "" );
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), complement, "" );
	m_IRBuilder.CreateStore( result, &m_registerP );
}

void Recompiler::PerformBvc( const std::string& labelName )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x40, true ) ), "" );

	auto cond = m_IRBuilder.CreateICmpNE( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x40, true ) ) );
	auto search = m_LabelNamesToBasicBlocks.find( labelName );
	if ( search != m_LabelNamesToBasicBlocks.end() )
	{
		llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
		if ( m_CurrentBasicBlock )
		{
			elseBlock->moveAfter( m_CurrentBasicBlock );
		}
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
		m_IRBuilder.CreateCondBr( cond, search->second, elseBlock );
		SelectBlock( elseBlock );
	}
	else
	{
		m_IRBuilder.CreateBr( m_PanicBlock );
	}
}

void Recompiler::PerformBvs( const std::string& labelName )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x40, true ) ), "" );

	auto cond = m_IRBuilder.CreateICmpEQ( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x40, true ) ) );
	auto search = m_LabelNamesToBasicBlocks.find( labelName );
	if ( search != m_LabelNamesToBasicBlocks.end() )
	{
		llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
		if ( m_CurrentBasicBlock )
		{
			elseBlock->moveAfter( m_CurrentBasicBlock );
		}
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
		m_IRBuilder.CreateCondBr( cond, search->second, elseBlock );
		SelectBlock( elseBlock );
	}
	else
	{
		m_IRBuilder.CreateBr( m_PanicBlock );
	}
}

void Recompiler::PerformBpl( const std::string& labelName )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x80, true ) ), "" );

	auto cond = m_IRBuilder.CreateICmpNE( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x80, true ) ) );
	auto search = m_LabelNamesToBasicBlocks.find( labelName );
	if ( search != m_LabelNamesToBasicBlocks.end() )
	{
		llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
		if ( m_CurrentBasicBlock )
		{
			elseBlock->moveAfter( m_CurrentBasicBlock );
		}
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
		m_IRBuilder.CreateCondBr( cond, search->second, elseBlock );
		SelectBlock( elseBlock );
	}
	else
	{
		m_IRBuilder.CreateBr( m_PanicBlock );
	}
}

void Recompiler::PerformBmi( const std::string& labelName )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x80, true ) ), "" );

	auto cond = m_IRBuilder.CreateICmpEQ( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x80, true ) ) );
	auto search = m_LabelNamesToBasicBlocks.find( labelName );
	if ( search != m_LabelNamesToBasicBlocks.end() )
	{
		llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
		if ( m_CurrentBasicBlock )
		{
			elseBlock->moveAfter( m_CurrentBasicBlock );
		}
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
		m_IRBuilder.CreateCondBr( cond, search->second, elseBlock );
		SelectBlock( elseBlock );
	}
	else
	{
		m_IRBuilder.CreateBr( m_PanicBlock );
	}
}

void Recompiler::PerformBne( const std::string& labelName )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x2, true ) ), "" );

	auto cond = m_IRBuilder.CreateICmpNE( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x2, true ) ) );
	auto search = m_LabelNamesToBasicBlocks.find( labelName );
	if ( search != m_LabelNamesToBasicBlocks.end() )
	{
		llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
		if ( m_CurrentBasicBlock )
		{
			elseBlock->moveAfter( m_CurrentBasicBlock );
		}
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
		m_IRBuilder.CreateCondBr( cond, search->second, elseBlock );
		SelectBlock( elseBlock );
	}
	else
	{
		m_IRBuilder.CreateBr( m_PanicBlock );
	}
}

void Recompiler::PerformBeq( const std::string& labelName )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x2, true ) ), "" );

	auto cond = m_IRBuilder.CreateICmpEQ( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x2, true ) ) );
	auto search = m_LabelNamesToBasicBlocks.find( labelName );
	if ( search != m_LabelNamesToBasicBlocks.end() )
	{
		llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
		if ( m_CurrentBasicBlock )
		{
			elseBlock->moveAfter( m_CurrentBasicBlock );
		}
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
		m_IRBuilder.CreateCondBr( cond, search->second, elseBlock );
		SelectBlock( elseBlock );
	}
	else
	{
		m_IRBuilder.CreateBr( m_PanicBlock );
	}
}

void Recompiler::PerformBcs( const std::string& labelName )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x1, true ) ), "" );

	auto cond = m_IRBuilder.CreateICmpEQ( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x1, true ) ) );
	auto search = m_LabelNamesToBasicBlocks.find( labelName );
	if ( search != m_LabelNamesToBasicBlocks.end() )
	{
		llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
		if ( m_CurrentBasicBlock )
		{
			elseBlock->moveAfter( m_CurrentBasicBlock );
		}
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
		m_IRBuilder.CreateCondBr( cond, search->second, elseBlock );
		SelectBlock( elseBlock );
	}
	else
	{
		m_IRBuilder.CreateBr( m_PanicBlock );
	}
}

void Recompiler::PerformBcc( const std::string& labelName )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x1, true ) ), "" );

	auto cond = m_IRBuilder.CreateICmpNE( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x1, true ) ) );
	auto search = m_LabelNamesToBasicBlocks.find( labelName );
	if ( search != m_LabelNamesToBasicBlocks.end() )
	{
		llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
		if ( m_CurrentBasicBlock )
		{
			elseBlock->moveAfter( m_CurrentBasicBlock );
		}
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
		m_IRBuilder.CreateCondBr( cond, search->second, elseBlock );
		SelectBlock( elseBlock );
	}
	else
	{
		m_IRBuilder.CreateBr( m_PanicBlock );
	}
}

void Recompiler::PerformJsl( const std::string& labelName )
{
	PushByteToStack( m_IRBuilder.CreateLoad( &m_registerPB, "" ) );
	auto pcPlus3 = m_IRBuilder.CreateAdd( m_IRBuilder.CreateLoad( &m_registerPC, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 3, false ) ), "" );
	PushWordToStack( pcPlus3 );
	auto search = m_LabelNamesToBasicBlocks.find( labelName );
	if ( search != m_LabelNamesToBasicBlocks.end() )
	{
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
		m_IRBuilder.CreateBr( search->second );
	}
	else
	{
		m_IRBuilder.CreateBr( m_PanicBlock );
	}

	m_CurrentBasicBlock = nullptr;
}

void Recompiler::PerformJsr( const std::string& labelName )
{
	auto pcPlus2 = m_IRBuilder.CreateAdd( m_IRBuilder.CreateLoad( &m_registerPC, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 2, false ) ), "" );
	PushWordToStack( pcPlus2  );
	auto search = m_LabelNamesToBasicBlocks.find( labelName );
	if ( search != m_LabelNamesToBasicBlocks.end() )
	{
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
		m_IRBuilder.CreateBr( search->second );
	}
	else
	{
		m_IRBuilder.CreateBr( m_PanicBlock );
	}

	m_CurrentBasicBlock = nullptr;
}

void Recompiler::PerformJmp( const std::string& labelName )
{
	auto search = m_LabelNamesToBasicBlocks.find( labelName );
	if ( search != m_LabelNamesToBasicBlocks.end() )
	{
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
		m_IRBuilder.CreateBr( search->second );
		m_CurrentBasicBlock = nullptr;
	}
	else
	{
		m_IRBuilder.CreateBr( m_PanicBlock );
	}
}

void Recompiler::PerformBra( const std::string& labelName )
{
	PerformJmp( labelName );
	m_CurrentBasicBlock = nullptr;
}

void Recompiler::PerformPea( llvm::Value* value )
{
	PushWordToStack( value );
}

void Recompiler::PerformRti()
{
	auto p = PullByteFromStack();
	auto pc = PullWordFromStack();
	auto pb = PullByteFromStack();

	m_IRBuilder.CreateStore( p, &m_registerP );
	m_IRBuilder.CreateStore( pc, &m_registerPC );
	m_IRBuilder.CreateStore( pb, &m_registerPB );
	PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
	m_IRBuilder.CreateRetVoid();
	m_CurrentBasicBlock = nullptr;
}

void Recompiler::PerformRtl()
{
	auto addr = PullWordFromStack();
	auto pc = m_IRBuilder.CreateAdd( addr, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 1, false ) ), "" );
	m_IRBuilder.CreateStore( pc, &m_registerPC );
	auto pb = PullByteFromStack();
	m_IRBuilder.CreateStore( pb, &m_registerPB );
	PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
	m_IRBuilder.CreateBr( m_DynamicJumpTableBlock );
	m_CurrentBasicBlock = nullptr;
}

void Recompiler::PerformRts()
{
	auto addr = PullWordFromStack();
	auto pc = m_IRBuilder.CreateAdd( addr, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 1, false ) ), "" );
	m_IRBuilder.CreateStore( pc, &m_registerPC );
	PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
	m_IRBuilder.CreateBr( m_DynamicJumpTableBlock );
	m_CurrentBasicBlock = nullptr;
}

llvm::Value* Recompiler::PullByteFromStack()
{
	// increment stack pointer
	auto sp = m_IRBuilder.CreateLoad( &m_registerSP, "" );
	auto spPlusOne = m_IRBuilder.CreateAdd( sp, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 1, false ) ), "" );
	m_IRBuilder.CreateStore( spPlusOne, &m_registerSP );
	// read the value at stack pointer
	std::vector<llvm::Value*> idxList = { llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0, false ) ), spPlusOne };
	auto ptr = m_IRBuilder.CreateInBoundsGEP( m_wRam.getType()->getPointerElementType(), &m_wRam, idxList );
	return m_IRBuilder.CreateLoad( ptr, "" );
}

llvm::Value* Recompiler::PullWordFromStack()
{
	auto low = PullByteFromStack();
	auto high = PullByteFromStack();
	auto low16 = m_IRBuilder.CreateZExt( low, llvm::Type::getInt16Ty( m_LLVMContext ), "" );
	auto high16 = m_IRBuilder.CreateZExt( high, llvm::Type::getInt16Ty( m_LLVMContext ), "" );
	auto word = m_IRBuilder.CreateShl( high16, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 8, false ) ), "" );
	return m_IRBuilder.CreateOr( word, low16, "" );
}

void Recompiler::PerformPhb( void )
{
	PushByteToStack( m_IRBuilder.CreateLoad( &m_registerDB, "" ) );
}

void Recompiler::PerformPhd( void )
{
	PushWordToStack( m_IRBuilder.CreateLoad( &m_registerDP, "" ) );
}

void Recompiler::PerformPhk( void )
{
	PushByteToStack( m_IRBuilder.CreateLoad( &m_registerPB, "" ) );
}

void Recompiler::PerformPhp( void )
{
	PushByteToStack( m_IRBuilder.CreateLoad( &m_registerP, "" ) );
}

void Recompiler::PerformPlb( void )
{
	auto stackValue = PullByteFromStack();
	m_IRBuilder.CreateStore( stackValue, &m_registerPB );
	TestAndSetZero8( stackValue );
	TestAndSetNegative8( stackValue );
}

void Recompiler::PerformPld( void )
{
	m_IRBuilder.CreateStore( PullWordFromStack(), &m_registerDP );
	TestAndSetZero16( &m_registerDP );
	TestAndSetNegative16( &m_registerDP );
}

void Recompiler::PerformInx( void )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x10, true ) ), "" );
	auto cond = m_IRBuilder.CreateICmpEQ( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x0, true ) ) );
	llvm::BasicBlock* thenBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	llvm::BasicBlock* endBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	if ( m_CurrentBasicBlock )
	{
		thenBlock->moveAfter( m_CurrentBasicBlock );
		elseBlock->moveAfter( thenBlock );
		endBlock->moveAfter( elseBlock );
	}
	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto newX16 =  PerformInx16Acc();
	TestAndSetZero16( newX16 );
	TestAndSetNegative16( newX16 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto newX8 = PerformInx8Acc();
	TestAndSetZero8( newX8 );
	TestAndSetNegative8( newX8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

llvm::Value* Recompiler::PerformInx16Acc( void )
{
	auto v = m_IRBuilder.CreateAdd( m_IRBuilder.CreateLoad( &m_registerX, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 1, false ) ), "" );
	m_IRBuilder.CreateStore( v, &m_registerX );
	return v;
}

llvm::Value* Recompiler::PerformInx8Acc( void )
{
	auto v = m_IRBuilder.CreateAdd( m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerX, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ), "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 1, false ) ), "" );
	m_IRBuilder.CreateStore( v, m_IRBuilder.CreateBitCast( &m_registerX, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	return v;
}

void Recompiler::PerformDex( void )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x10, true ) ), "" );
	auto cond = m_IRBuilder.CreateICmpEQ( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x0, true ) ) );
	llvm::BasicBlock* thenBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	llvm::BasicBlock* endBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	if ( m_CurrentBasicBlock )
	{
		thenBlock->moveAfter( m_CurrentBasicBlock );
		elseBlock->moveAfter( thenBlock );
		endBlock->moveAfter( elseBlock );
	}
	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto newX16 =  PerformDex16Acc();
	TestAndSetZero16( newX16 );
	TestAndSetNegative16( newX16 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto newX8 = PerformDex8Acc();
	TestAndSetZero8( newX8 );
	TestAndSetNegative8( newX8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

llvm::Value* Recompiler::PerformDex16Acc( void )
{
	auto v = m_IRBuilder.CreateSub( m_IRBuilder.CreateLoad( &m_registerX, ""), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 1, false ) ), "" );
	m_IRBuilder.CreateStore( v, &m_registerX );
	return v;
}

llvm::Value* Recompiler::PerformDex8Acc( void )
{
	auto v = m_IRBuilder.CreateSub( m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerX, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ), ""), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 1, false ) ), "" );
	m_IRBuilder.CreateStore( v, m_IRBuilder.CreateBitCast( &m_registerX, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	return v;
}

void Recompiler::PerformInc( void )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x20, true ) ), "" );
	auto cond = m_IRBuilder.CreateICmpEQ( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x0, true ) ) );
	llvm::BasicBlock* thenBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	llvm::BasicBlock* endBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	if ( m_CurrentBasicBlock )
	{
		thenBlock->moveAfter( m_CurrentBasicBlock );
		elseBlock->moveAfter( thenBlock );
		endBlock->moveAfter( elseBlock );
	}
	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto newA16 = PerformInc16Acc();
	TestAndSetZero16( newA16 );
	TestAndSetNegative16( newA16 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto newA8 = PerformInc8Acc();
	TestAndSetZero8( newA8 );
	TestAndSetNegative8( newA8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

llvm::Value* Recompiler::PerformInc16Acc( void )
{
	auto v = m_IRBuilder.CreateAdd( m_IRBuilder.CreateLoad( &m_registerA, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 1, false ) ), "" );
	m_IRBuilder.CreateStore( v, &m_registerA );
	return v;
}

llvm::Value* Recompiler::PerformInc8Acc( void )
{
	auto v = m_IRBuilder.CreateAdd( m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ), "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 1, false ) ), "" );
	m_IRBuilder.CreateStore( v, m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	return v;
}

void Recompiler::PerformDec( void )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), 0x20, "" );
	auto cond = m_IRBuilder.CreateICmpEQ( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x0, true ) ) );
	llvm::BasicBlock* thenBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	llvm::BasicBlock* endBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	if ( m_CurrentBasicBlock )
	{
		thenBlock->moveAfter( m_CurrentBasicBlock );
		elseBlock->moveAfter( thenBlock );
		endBlock->moveAfter( elseBlock );
	}
	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto newA16 = PerformDec16Acc();
	TestAndSetZero16( newA16 );
	TestAndSetNegative16( newA16 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto newA8 = PerformDec8Acc();
	TestAndSetZero8( newA8 );
	TestAndSetNegative8( newA8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

llvm::Value* Recompiler::PerformDec16Acc( void )
{
	auto v = m_IRBuilder.CreateSub( m_IRBuilder.CreateLoad( &m_registerA, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 1, false ) ), "" );
	m_IRBuilder.CreateStore( v, &m_registerA );
	return v;
}

llvm::Value* Recompiler::PerformDec8Acc( void )
{
	auto v = m_IRBuilder.CreateSub( m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ), "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 1, false ) ), "" );
	m_IRBuilder.CreateStore( v, m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	return v;
}

void Recompiler::PerformIny( void )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x10, true ) ), "" );
	auto cond = m_IRBuilder.CreateICmpEQ( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x0, true ) ) );
	llvm::BasicBlock* thenBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	llvm::BasicBlock* endBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	if ( m_CurrentBasicBlock )
	{
		thenBlock->moveAfter( m_CurrentBasicBlock );
		elseBlock->moveAfter( thenBlock );
		endBlock->moveAfter( elseBlock );
	}
	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto newY16 = PerformIny16Acc();
	TestAndSetZero16( newY16 );
	TestAndSetNegative16( newY16 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto newY8 = PerformIny8Acc();
	TestAndSetZero8( newY8 );
	TestAndSetNegative8( newY8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

llvm::Value* Recompiler::PerformIny16Acc( void )
{
	auto v = m_IRBuilder.CreateAdd( m_IRBuilder.CreateLoad( &m_registerY, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 1, false ) ), "" );
	m_IRBuilder.CreateStore( v, &m_registerY );
	return v;
}

llvm::Value* Recompiler::PerformIny8Acc( void )
{
	auto v = m_IRBuilder.CreateAdd( m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerY, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ), "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 1, false ) ), "" );
	m_IRBuilder.CreateStore( v, m_IRBuilder.CreateBitCast( &m_registerY, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	return v;
}

void Recompiler::PerformDey( void )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x10, true ) ), "" );
	auto cond = m_IRBuilder.CreateICmpEQ( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x0, true ) ) );
	llvm::BasicBlock* thenBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	llvm::BasicBlock* endBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	if ( m_CurrentBasicBlock )
	{
		thenBlock->moveAfter( m_CurrentBasicBlock );
		elseBlock->moveAfter( thenBlock );
		endBlock->moveAfter( elseBlock );
	}
	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto newY16 = PerformDey16Acc();
	TestAndSetZero16( newY16 );
	TestAndSetNegative16( newY16 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto newY8 = PerformDey8Acc();
	TestAndSetZero8( newY8 );
	TestAndSetNegative8( newY8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

llvm::Value* Recompiler::PerformDey16Acc( void )
{
	auto v = m_IRBuilder.CreateSub( m_IRBuilder.CreateLoad( &m_registerY, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 1, false ) ), "" );
	m_IRBuilder.CreateStore( v, &m_registerY );
	return v;
}

llvm::Value* Recompiler::PerformDey8Acc( void )
{
	auto v = m_IRBuilder.CreateSub( m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerY, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ), "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 1, false ) ), "" );
	m_IRBuilder.CreateStore( v, m_IRBuilder.CreateBitCast( &m_registerY, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	return v;
}

void Recompiler::PerformAsl( void )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x20, true ) ), "" );
	auto cond = m_IRBuilder.CreateICmpEQ( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x0, true ) ) );
	llvm::BasicBlock* thenBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	llvm::BasicBlock* endBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	if ( m_CurrentBasicBlock )
	{
		thenBlock->moveAfter( m_CurrentBasicBlock );
		elseBlock->moveAfter( thenBlock );
		endBlock->moveAfter( elseBlock );
	}
	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto newA16 = PerformAsl16Acc();
	TestAndSetZero16( newA16 );
	TestAndSetNegative16( newA16 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto newA8 = PerformAsl8Acc();
	TestAndSetZero8( newA8 );
	TestAndSetNegative8( newA8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

llvm::Value* Recompiler::PerformAsl16Acc( void )
{
	llvm::Value* x8000 = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0x8000, false ) );
	llvm::Value* masked = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerA, "" ), x8000, "" );
	llvm::Value* carry = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_EQ, masked, x8000, "" );
	m_IRBuilder.CreateStore( carry, m_IRBuilder.CreateBitCast( &m_registerP, llvm::Type::getInt1PtrTy( m_LLVMContext ), "" ) );
	auto v = m_IRBuilder.CreateShl( m_IRBuilder.CreateLoad( &m_registerA, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 1, false ) ), "" );
	m_IRBuilder.CreateStore( v, &m_registerA );
	return v;
}

llvm::Value* Recompiler::PerformAsl8Acc( void )
{
	llvm::Value* x80 = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x80, false ) );
	llvm::Value* masked = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ), "" ), x80, "" );
	llvm::Value* carry = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_EQ, masked, x80, "" );
	m_IRBuilder.CreateStore( carry, m_IRBuilder.CreateBitCast( &m_registerP, llvm::Type::getInt1PtrTy( m_LLVMContext ), "" ) );
	auto v = m_IRBuilder.CreateShl( m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ), "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 1, false ) ), "" );
	m_IRBuilder.CreateStore( v, m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	return v;
}

void Recompiler::PerformLsr( void )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x20, true ) ), "" );
	auto cond = m_IRBuilder.CreateICmpEQ( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x0, true ) ) );
	llvm::BasicBlock* thenBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	llvm::BasicBlock* endBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	if ( m_CurrentBasicBlock )
	{
		thenBlock->moveAfter( m_CurrentBasicBlock );
		elseBlock->moveAfter( thenBlock );
		endBlock->moveAfter( elseBlock );
	}
	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto newA16 =  PerformLsr16Acc();
	TestAndSetZero16( newA16 );
	TestAndSetNegative16( newA16 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto newA8 = PerformLsr8Acc();
	TestAndSetZero8( newA8 );
	TestAndSetNegative8( newA8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

llvm::Value* Recompiler::PerformLsr16Acc( void )
{
	llvm::Value* x1 = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0x1, false ) );
	llvm::Value* masked = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerA, "" ), x1, "" );
	llvm::Value* carry = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_EQ, masked, x1, "" );
	m_IRBuilder.CreateStore( carry, m_IRBuilder.CreateBitCast( &m_registerP, llvm::Type::getInt1PtrTy( m_LLVMContext ), "" ) );
	auto v = m_IRBuilder.CreateLShr( m_IRBuilder.CreateLoad( &m_registerA, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 1, false ) ), "" );
	m_IRBuilder.CreateStore( v, &m_registerA );
	return v;
}

llvm::Value* Recompiler::PerformLsr8Acc( void )
{
	llvm::Value* x1 = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x1, false ) );
	llvm::Value* masked = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ), "" ), x1, "" );
	llvm::Value* carry = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_EQ, masked, x1, "" );
	m_IRBuilder.CreateStore( carry, m_IRBuilder.CreateBitCast( &m_registerP, llvm::Type::getInt1PtrTy( m_LLVMContext ), "" ) );
	auto v = m_IRBuilder.CreateLShr( m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ), "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 1, false ) ), "" );
	m_IRBuilder.CreateStore( v, m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	return v;
}

void Recompiler::PerformRol( void )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x20, true ) ), "" );
	auto cond = m_IRBuilder.CreateICmpEQ( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x0, true ) ) );
	llvm::BasicBlock* thenBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	llvm::BasicBlock* endBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	if ( m_CurrentBasicBlock )
	{
		thenBlock->moveAfter( m_CurrentBasicBlock );
		elseBlock->moveAfter( thenBlock );
		endBlock->moveAfter( elseBlock );
	}
	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto newA16 = PerformRol16Acc();
	TestAndSetZero16( newA16 );
	TestAndSetNegative16( newA16 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto newA8 = PerformRol8Acc();
	TestAndSetZero8( newA8 );
	TestAndSetNegative8( newA8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

llvm::Value* Recompiler::PerformRol16Acc( void )
{
	llvm::Value* x1 = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0x1, false ) );
	llvm::Value* masked = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerA, "" ), x1, "" );
	llvm::Value* carry = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_EQ, masked, x1, "" );

	llvm::Value* carryExt = m_IRBuilder.CreateZExt( carry, llvm::Type::getInt32Ty( m_LLVMContext ), "" );
	llvm::Value* aExt = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerA, "" ), llvm::Type::getInt32Ty( m_LLVMContext ), "" );
	auto newValueBeforeShift = m_IRBuilder.CreateOr( aExt, carryExt, "" );
	auto shifted = m_IRBuilder.CreateShl( newValueBeforeShift, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 1, false ) ), "" );
	auto newCarry = m_IRBuilder.CreateICmpUGE( shifted, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x10000, false ) ), "" );
	m_IRBuilder.CreateStore( newCarry, m_IRBuilder.CreateBitCast( &m_registerP, llvm::Type::getInt1PtrTy( m_LLVMContext ), "" ) );

	auto newA = m_IRBuilder.CreateTrunc( shifted, llvm::Type::getInt16Ty( m_LLVMContext ) );
	m_IRBuilder.CreateStore( newA, &m_registerA, "" );
	return newA;
}

llvm::Value* Recompiler::PerformRol8Acc( void )
{
	llvm::Value* x1 = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x1, false ) );
	llvm::Value* masked = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ), "" ), x1, "" );
	llvm::Value* carry = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_EQ, masked, x1, "" );

	llvm::Value* carryExt = m_IRBuilder.CreateZExt( carry, llvm::Type::getInt16Ty( m_LLVMContext ), "" );
	llvm::Value* aExt = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ), "" ), llvm::Type::getInt16Ty( m_LLVMContext ), "" );
	auto newValueBeforeShift = m_IRBuilder.CreateOr( aExt, carryExt, "" );
	auto shifted = m_IRBuilder.CreateShl( newValueBeforeShift, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 1, false ) ), "" );
	auto newCarry = m_IRBuilder.CreateICmpUGE( shifted, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0x00100, false ) ), "" );
	m_IRBuilder.CreateStore( newCarry, m_IRBuilder.CreateBitCast( &m_registerP, llvm::Type::getInt1PtrTy( m_LLVMContext ), "" ) );

	auto newA = m_IRBuilder.CreateTrunc( shifted, llvm::Type::getInt8Ty( m_LLVMContext ) );
	m_IRBuilder.CreateStore( newA, m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ) ), "" );
	return newA;
}

void Recompiler::PerformRor( void )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x20, true ) ), "" );
	auto cond = m_IRBuilder.CreateICmpEQ( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x0, true ) ) );
	llvm::BasicBlock* thenBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	llvm::BasicBlock* endBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	if ( m_CurrentBasicBlock )
	{
		thenBlock->moveAfter( m_CurrentBasicBlock );
		elseBlock->moveAfter( thenBlock );
		endBlock->moveAfter( elseBlock );
	}
	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto newA16 = PerformRor16Acc();
	TestAndSetZero16( newA16 );
	TestAndSetNegative16( newA16 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto newA8 = PerformRor8Acc();
	TestAndSetZero8( newA8 );
	TestAndSetNegative8( newA8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

llvm::Value* Recompiler::PerformRor16Acc( void )
{
	llvm::Value* x1 = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0x1, false ) );
	llvm::Value* masked = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerA, "" ), x1, "" );
	llvm::Value* carry = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_EQ, masked, x1, "" );

	llvm::Value* carryExt = m_IRBuilder.CreateZExt( carry, llvm::Type::getInt32Ty( m_LLVMContext ), "" );
	llvm::Value* carryExtShifted = m_IRBuilder.CreateShl( carryExt, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 16, false ) ), "" );

	llvm::Value* aExt = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerA, "" ), llvm::Type::getInt32Ty( m_LLVMContext ), "" );
	auto newValueBeforeShift = m_IRBuilder.CreateOr( aExt, carryExtShifted, "" );
	auto newCarry = m_IRBuilder.CreateICmpNE( m_IRBuilder.CreateAnd( newValueBeforeShift, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x1, false ) ), "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0, false ) ), "" );

	auto shifted = m_IRBuilder.CreateLShr( newValueBeforeShift, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 1, false ) ), "" );
	m_IRBuilder.CreateStore( newCarry, m_IRBuilder.CreateBitCast( &m_registerP, llvm::Type::getInt1PtrTy( m_LLVMContext ), "" ) );
	auto newA = m_IRBuilder.CreateTrunc( shifted, llvm::Type::getInt16Ty( m_LLVMContext ) );
	m_IRBuilder.CreateStore( newA, &m_registerA, "" );
	return newA;
}

llvm::Value* Recompiler::PerformRor8Acc( void )
{
	llvm::Value* x1 = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x1, false ) );
	llvm::Value* masked = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ), "" ), x1, "" );
	llvm::Value* carry = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_EQ, masked, x1, "" );

	llvm::Value* carryExt = m_IRBuilder.CreateZExt( carry, llvm::Type::getInt16Ty( m_LLVMContext ), "" );
	llvm::Value* carryExtShifted = m_IRBuilder.CreateShl( carryExt, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 8, false ) ), "" );

	llvm::Value* aExt = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ), "" ), llvm::Type::getInt16Ty( m_LLVMContext ), "" );
	auto newValueBeforeShift = m_IRBuilder.CreateOr( aExt, carryExtShifted, "" );

	auto newCarry = m_IRBuilder.CreateICmpNE( m_IRBuilder.CreateAnd( newValueBeforeShift, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0x1, false ) ), "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0, false ) ), "" );
	auto shifted = m_IRBuilder.CreateLShr( newValueBeforeShift, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 1, false ) ), "" );
	m_IRBuilder.CreateStore( newCarry, m_IRBuilder.CreateBitCast( &m_registerP, llvm::Type::getInt1PtrTy( m_LLVMContext ), "" ) );

	auto newA = m_IRBuilder.CreateTrunc( shifted, llvm::Type::getInt8Ty( m_LLVMContext ) );
	m_IRBuilder.CreateStore( newA, m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ) ), "" );
	return newA;
}

void Recompiler::PerformPlp( void )
{
	m_IRBuilder.CreateStore( PullByteFromStack(), &m_registerP );
}

void Recompiler::PushByteToStack( llvm::Value* value )
{
	// write the value to the address at current stack pointer
	auto sp = m_IRBuilder.CreateLoad( &m_registerSP, "" );
	std::vector<llvm::Value*> idxList = { llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0, false ) ), sp };
	auto ptr = m_IRBuilder.CreateInBoundsGEP( m_wRam.getType()->getPointerElementType(), &m_wRam, idxList );
	m_IRBuilder.CreateStore( value, ptr );

	// stack pointer = stack pointer - 1
	auto spMinusOne = m_IRBuilder.CreateSub( sp, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 1, false ) ), "" );
	m_IRBuilder.CreateStore( spMinusOne, &m_registerSP );
}

void Recompiler::PushWordToStack( llvm::Value* value )
{
	auto high16 = m_IRBuilder.CreateShl( value, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 8, false ) ), "" );
	auto high = m_IRBuilder.CreateTrunc( high16, llvm::Type::getInt8Ty( m_LLVMContext ), "" );
	PushByteToStack( high );

	auto low16 = m_IRBuilder.CreateAnd( value, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0xff, false ) ), "" );
	auto low= m_IRBuilder.CreateTrunc( low16, llvm::Type::getInt8Ty( m_LLVMContext ), "" );
	PushByteToStack( low );
}

void Recompiler::PerformTcs()
{
	auto result = m_IRBuilder.CreateLoad( &m_registerA, "" );
	m_IRBuilder.CreateStore( result, &m_registerSP );
	TestAndSetZero16( result );
	TestAndSetNegative16( result );
}

void Recompiler::PerformTcd()
{
	auto result = m_IRBuilder.CreateLoad( &m_registerA, "" );
	m_IRBuilder.CreateStore( result, &m_registerDP );
	TestAndSetZero16( result );
	TestAndSetNegative16( result );
}

void Recompiler::PerformTdc()
{
	auto result = m_IRBuilder.CreateLoad( &m_registerDP, "" );
	m_IRBuilder.CreateStore( result, &m_registerA );
	TestAndSetZero16( result );
	TestAndSetNegative16( result );
}

void Recompiler::PerformTsc()
{
	auto result = m_IRBuilder.CreateLoad( &m_registerSP, "" );
	m_IRBuilder.CreateStore( result, &m_registerA );
	TestAndSetZero16( result );
	TestAndSetNegative16( result );
}

void Recompiler::PerformXba()
{
	llvm::Value* a8BeforeSwap = CreateLoadA8();
	llvm::Value* b8BeforeSwap = CreateLoadB8();

	m_IRBuilder.CreateStore( b8BeforeSwap, m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );

	auto a = m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" );
	auto b8Ptr = m_IRBuilder.CreateInBoundsGEP( llvm::Type::getInt8Ty( m_LLVMContext ), a, llvm::ConstantInt::get( llvm::Type::getInt32Ty( m_LLVMContext ), 1 ) );

	m_IRBuilder.CreateStore( a8BeforeSwap, b8Ptr );
}

llvm::Value* Recompiler::CreateLoadA16( void )
{
	return m_IRBuilder.CreateLoad( &m_registerA, "" );
}

llvm::Value* Recompiler::CreateLoadA8( void )
{
	return m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ), "" );
}

llvm::Value* Recompiler::CreateLoadB8( void )
{
	llvm::Value* indexList[ 2 ] = { llvm::ConstantInt::get( llvm::Type::getInt32Ty( m_LLVMContext ), 0 ), llvm::ConstantInt::get( llvm::Type::getInt32Ty( m_LLVMContext ), 1 ) };
	auto v = m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" );
	auto a = m_IRBuilder.CreateInBoundsGEP( llvm::Type::getInt8Ty( m_LLVMContext ), v, llvm::ConstantInt::get( llvm::Type::getInt32Ty( m_LLVMContext ), 1 ) );
	return m_IRBuilder.CreateLoad( a, "" );
}

llvm::Value* Recompiler::CreateLoadX16( void )
{
	return m_IRBuilder.CreateLoad( &m_registerX, "" );
}

llvm::Value* Recompiler::CreateLoadX8( void )
{
	return m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerX, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ), "" );
}

llvm::Value* Recompiler::CreateLoadY16( void )
{
	return m_IRBuilder.CreateLoad( &m_registerY, "" );
}

llvm::Value* Recompiler::CreateLoadY8( void )
{
	return m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerY, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ), "" );
}

void Recompiler::ClearCarry()
{
	llvm::Value* mask = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( ~0x1 ), false ) );
	auto masked = m_IRBuilder.CreateAnd( mask, m_IRBuilder.CreateLoad( &m_registerP, "" ), "" );
	m_IRBuilder.CreateStore( masked, &m_registerP );
}

void Recompiler::SetCarry()
{
	llvm::Value* mask = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( 0x1 ), false ) );
	auto masked = m_IRBuilder.CreateOr( mask, m_IRBuilder.CreateLoad( &m_registerP, "" ), "" );
	m_IRBuilder.CreateStore( masked, &m_registerP );
}

void Recompiler::ClearDecimal()
{
	llvm::Value* mask = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( ~( 1 << 3) ), false ) );
	auto masked = m_IRBuilder.CreateAnd( mask, m_IRBuilder.CreateLoad( &m_registerP, "" ), "" );
	m_IRBuilder.CreateStore( masked, &m_registerP );
}

void Recompiler::SetDecimal()
{
	llvm::Value* mask = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( 1 << 3 ), false ) );
	auto masked = m_IRBuilder.CreateOr( mask, m_IRBuilder.CreateLoad( &m_registerP, "" ), "" );
	m_IRBuilder.CreateStore( masked, &m_registerP );
}

void Recompiler::ClearInterrupt()
{
	llvm::Value* mask = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( ~( 1 << 2 ) ), false ) );
	auto masked = m_IRBuilder.CreateAnd( mask, m_IRBuilder.CreateLoad( &m_registerP, "" ), "" );
	m_IRBuilder.CreateStore( masked, &m_registerP );
}

void Recompiler::SetInterrupt()
{
	llvm::Value* mask = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( 1 << 2 ), false ) );
	auto masked = m_IRBuilder.CreateOr( mask, m_IRBuilder.CreateLoad( &m_registerP, "" ), "" );
	m_IRBuilder.CreateStore( masked, &m_registerP );
}

void Recompiler::ClearOverflow()
{
	llvm::Value* mask = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( ~( 1 << 6 ) ), false ) );
	auto masked = m_IRBuilder.CreateAnd( mask, m_IRBuilder.CreateLoad( &m_registerP, "" ), "" );
	m_IRBuilder.CreateStore( masked, &m_registerP );
}

void Recompiler::PerformCmp16( llvm::Value* lValue, llvm::Value* rValue )
{
	llvm::Value* diff = m_IRBuilder.CreateSub( lValue, rValue, "" );
	TestAndSetZero16( diff );
	TestAndSetNegative16( diff );
	TestAndSetCarrySubtraction( lValue, rValue );
}

void Recompiler::PerformCmp8( llvm::Value* lValue, llvm::Value* rValue )
{
	llvm::Value* diff = m_IRBuilder.CreateSub( lValue, rValue, "" );
	TestAndSetZero8( diff );
	TestAndSetNegative8( diff );
	TestAndSetCarrySubtraction( lValue, rValue );
}

void Recompiler::TestAndSetCarrySubtraction( llvm::Value* lValue, llvm::Value* rValue )
{
	llvm::Value* isCarry = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_UGE, lValue, rValue, "" );
	auto zExtCarry = m_IRBuilder.CreateZExt( isCarry, llvm::Type::getInt8Ty( m_LLVMContext ) );
	auto newP = m_IRBuilder.CreateOr( zExtCarry, m_IRBuilder.CreateLoad( &m_registerP, "" ) );
	m_IRBuilder.CreateStore( newP, &m_registerP );
}

void Recompiler::PerformLdy16( llvm::Value* value )
{
	m_IRBuilder.CreateStore( value, &m_registerY );
	TestAndSetZero16( value );
	TestAndSetNegative16( value );
}

void Recompiler::PerformLdy8( llvm::Value* value )
{
	llvm::Value* writeRegisterY8Bit = m_IRBuilder.CreateBitCast( &m_registerY, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" );
	m_IRBuilder.CreateStore( value, writeRegisterY8Bit );
	TestAndSetZero8( writeRegisterY8Bit );
	TestAndSetNegative8( writeRegisterY8Bit );
}

void Recompiler::PerformLdx16( llvm::Value* value )
{
	m_IRBuilder.CreateStore( value, &m_registerX );
	TestAndSetZero16( value );
	TestAndSetNegative16( value );
}

void Recompiler::PerformLdx8( llvm::Value* value )
{
	llvm::Value* writeRegisterX8Bit = m_IRBuilder.CreateBitCast( &m_registerX, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" );
	m_IRBuilder.CreateStore( value, writeRegisterX8Bit );
	TestAndSetZero8( writeRegisterX8Bit );
	TestAndSetNegative8( writeRegisterX8Bit );
}

void Recompiler::PerformLda16( llvm::Value* value )
{
	m_IRBuilder.CreateStore( value, &m_registerA );
	TestAndSetZero16( value );
	TestAndSetNegative16( value );
}

void Recompiler::PerformLda8( llvm::Value* value )
{
	llvm::Value* writeRegisterA8Bit = m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" );
	m_IRBuilder.CreateStore( value, writeRegisterA8Bit );
	TestAndSetZero8( value );
	TestAndSetNegative8( value );
}

void Recompiler::PerformOra16( llvm::Value* value )
{
	llvm::LoadInst* loadA = m_IRBuilder.CreateLoad( &m_registerA, "" );
	llvm::Value* newA = m_IRBuilder.CreateOr( loadA, value, "" );
	m_IRBuilder.CreateStore( newA, &m_registerA );
	TestAndSetZero16( newA );
	TestAndSetNegative16( newA );
}

void Recompiler::PerformOra8( llvm::Value* value )
{
	llvm::LoadInst* loadA = m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	llvm::Value* newA = m_IRBuilder.CreateOr( loadA, value, "" );
	m_IRBuilder.CreateStore( newA, m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	TestAndSetZero8( newA );
	TestAndSetNegative8( newA );
}

void Recompiler::PerformAnd16( llvm::Value* value )
{
	llvm::LoadInst* loadA = m_IRBuilder.CreateLoad( &m_registerA, "" );
	llvm::Value* newA = m_IRBuilder.CreateAnd( loadA, value, "" );
	m_IRBuilder.CreateStore( newA, &m_registerA );
	TestAndSetZero16( newA );
	TestAndSetNegative16( newA );
}

void Recompiler::PerformAnd8( llvm::Value* value )
{
	llvm::LoadInst* loadA = m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	llvm::Value* newA = m_IRBuilder.CreateAnd( loadA, value, "" );
	m_IRBuilder.CreateStore( newA, m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	TestAndSetZero8( newA );
	TestAndSetNegative8( newA );
}

void Recompiler::PerformEor16( llvm::Value* value )
{
	llvm::LoadInst* loadA = m_IRBuilder.CreateLoad( &m_registerA, "" );
	llvm::Value* newA = m_IRBuilder.CreateXor( loadA, value, "" );
	m_IRBuilder.CreateStore( newA, &m_registerA );
	TestAndSetZero16( newA );
	TestAndSetNegative16( newA );
}

void Recompiler::PerformEor8( llvm::Value* value )
{
	llvm::LoadInst* loadA = m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	llvm::Value* newA = m_IRBuilder.CreateXor( loadA, value, "" );
	m_IRBuilder.CreateStore( newA, m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	TestAndSetZero8( newA );
	TestAndSetNegative8( newA );
}

void Recompiler::TestAndSetZero16( llvm::Value* value )
{
	llvm::Value* zeroConst = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0, false ) );
	llvm::Value* isZero = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_EQ, value, zeroConst, "" );
	auto zExtIsZero = m_IRBuilder.CreateZExt( isZero, llvm::Type::getInt8Ty( m_LLVMContext ) );
	auto shiftedIsZero = m_IRBuilder.CreateShl( zExtIsZero, 1 );
	auto masked = m_IRBuilder.CreateOr( shiftedIsZero, m_IRBuilder.CreateLoad( &m_registerP, "" ) );

	m_IRBuilder.CreateStore( masked, &m_registerP );
}

void Recompiler::TestAndSetZero8( llvm::Value* value )
{
	llvm::Value* zeroConst = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0, false ) );
	llvm::Value* isZero = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_EQ, value, zeroConst, "" );
	auto zExtIsZero = m_IRBuilder.CreateZExt( isZero, llvm::Type::getInt8Ty( m_LLVMContext ) );
	auto shiftedIsZero = m_IRBuilder.CreateShl( zExtIsZero, 1 );
	auto masked = m_IRBuilder.CreateOr( shiftedIsZero, m_IRBuilder.CreateLoad( &m_registerP, "" ) );

	m_IRBuilder.CreateStore( masked, &m_registerP );
}

void Recompiler::TestAndSetNegative16( llvm::Value* value )
{
	llvm::Value* x8000 = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0x8000, false ) );
	llvm::Value* masked = m_IRBuilder.CreateAnd( value, x8000, "" );
	llvm::Value* isNeg = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_EQ, masked, x8000, "" );

	auto zExtIsNeg = m_IRBuilder.CreateZExt( isNeg, llvm::Type::getInt8Ty( m_LLVMContext ) );
	auto shiftedIsNeg = m_IRBuilder.CreateShl( zExtIsNeg, 7 );
	auto newP = m_IRBuilder.CreateOr( shiftedIsNeg, m_IRBuilder.CreateLoad( &m_registerP, "" ) );

	m_IRBuilder.CreateStore( newP, &m_registerP );
}

void Recompiler::TestAndSetNegative8( llvm::Value* value )
{
	llvm::Value* x80 = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x80, false ) );
	llvm::Value* masked = m_IRBuilder.CreateAnd( value, x80, "" );
	llvm::Value* isNeg = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_EQ, masked, x80, "" );

	auto zExtIsNeg = m_IRBuilder.CreateZExt( isNeg, llvm::Type::getInt8Ty( m_LLVMContext ) );
	auto shiftedIsNeg = m_IRBuilder.CreateShl( zExtIsNeg, 7 );
	auto newP = m_IRBuilder.CreateOr( shiftedIsNeg, m_IRBuilder.CreateLoad( &m_registerP, "" ) );

	m_IRBuilder.CreateStore( newP, &m_registerP );
}

void Recompiler::LoadAST( const char* filename )
{
	std::ifstream ifs( filename );
	if ( ifs.is_open() )
	{
		nlohmann::json j = nlohmann::json::parse( ifs );

		std::vector<nlohmann::json> ast;
		j[ "rom_reset_label_name" ].get_to( m_RomResetLabelName );
		j[ "rom_nmi_label_name" ].get_to( m_RomNmiLabelName );
		j[ "rom_irq_label_name" ].get_to( m_RomIrqLabelName );
		j[ "ast" ].get_to( ast );

		const uint32_t numNodes = ast.size();
		for ( uint32_t i = 0; i < numNodes; i++ )
		{
			auto&& current_node = ast[ i ];
			if ( current_node.contains( "Label" ) && ( i + 1 != numNodes ) )
			{
				m_Program.emplace_back( Label{ current_node[ "Label" ][ "name" ], current_node[ "Label" ][ "offset" ] } );
				m_LabelNamesToOffsets.emplace( current_node[ "Label" ][ "name" ], current_node[ "Label" ][ "offset" ] );
				m_OffsetsToLabelNames.emplace( current_node[ "Label" ][ "offset" ], current_node[ "Label" ][ "name" ] );
			}
			else if ( current_node.contains( "Instruction" ) )
			{
				if ( current_node[ "Instruction" ].contains( "operand" ) )
				{
					if ( current_node[ "Instruction" ].contains( "jump_label_name" ) )
					{
						m_Program.emplace_back( Instruction{ current_node[ "Instruction" ][ "offset" ], current_node[ "Instruction" ][ "opcode" ], current_node[ "Instruction" ][ "operand" ], current_node[ "Instruction" ][ "jump_label_name" ],  current_node[ "Instruction" ][ "operand_size" ],  current_node[ "Instruction" ][ "memory_mode" ], current_node[ "Instruction" ][ "index_mode" ] } );
					}
					else
					{
						m_Program.emplace_back( Instruction{ current_node[ "Instruction" ][ "offset" ], current_node[ "Instruction" ][ "opcode" ], current_node[ "Instruction" ][ "operand" ], current_node[ "Instruction" ][ "operand_size" ],  current_node[ "Instruction" ][ "memory_mode" ], current_node[ "Instruction" ][ "index_mode" ] } );
					}
				}
				else
				{
					m_Program.emplace_back( Instruction{ current_node[ "Instruction" ][ "offset" ], current_node[ "Instruction" ][ "opcode" ], current_node[ "Instruction" ][ "memory_mode" ], current_node[ "Instruction" ][ "index_mode" ] } );
				}
			}
		}
	}
	else
	{
		std::cerr << "Can't load ast file " << filename << std::endl;
	}
}

Recompiler::Label::Label( const std::string& name, const uint32_t offset )
	: m_Name( name )
	, m_Offset( offset )
{
}

Recompiler::Label::~Label()
{

}

Recompiler::Instruction::Instruction( const uint32_t offset, const uint8_t opcode, const uint32_t operand, const uint32_t operand_size, MemoryMode memoryMode, MemoryMode indexMode )
	: m_Offset( offset )
	, m_Opcode( opcode )
	, m_Operand( operand )
	, m_MemoryMode( memoryMode )
	, m_IndexMode( indexMode )
	, m_HasOperand( true )
{
}

Recompiler::Instruction::Instruction( const uint32_t offset, const uint8_t opcode, const uint32_t operand, const std::string& jumpLabelName, const uint32_t operand_size, MemoryMode memoryMode, MemoryMode indexMode )
	: m_Offset( offset )
	, m_Opcode( opcode )
	, m_Operand( operand )
	, m_JumpLabelName( jumpLabelName )
	, m_MemoryMode( memoryMode )
	, m_IndexMode( indexMode )
	, m_HasOperand( true )
{
}

Recompiler::Instruction::Instruction( const uint32_t offset, const uint8_t opcode, MemoryMode memoryMode, MemoryMode indexMode )
	: m_Offset( offset )
	, m_Opcode( opcode )
	, m_Operand( 0 )
	, m_MemoryMode( memoryMode )
	, m_IndexMode( indexMode )
	, m_HasOperand( false )
{
}

Recompiler::Instruction::~Instruction()
{

}
