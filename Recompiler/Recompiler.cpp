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
#include "../Utils.hpp"

Recompiler::Recompiler()
: m_IRBuilder( m_LLVMContext )
, m_RecompilationModule( "recompilation", m_LLVMContext )
, m_RomResetAddr( 0 )
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
, m_DynamicLoad8( m_RecompilationModule, llvm::Type::getInt8Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "DynamicLoad8" )
, m_DynamicLoad16( m_RecompilationModule, llvm::Type::getInt16Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "DynamicLoad16" )
, m_wRam( m_RecompilationModule, llvm::ArrayType::get( llvm::Type::getInt8Ty( m_LLVMContext ), WRAM_SIZE ), false, llvm::GlobalValue::ExternalLinkage, 0, "wRam" )
, m_Rom( m_RecompilationModule, llvm::ArrayType::get( llvm::Type::getInt8Ty( m_LLVMContext ), ROM_SIZE ), false, llvm::GlobalValue::ExternalLinkage, 0, "rom" )
, m_CurrentBasicBlock( nullptr )
, m_CycleFunction( nullptr )
, m_PanicFunction( nullptr )
, m_PanicBlock( nullptr )
, m_SPCWritePortFunction( nullptr )
, m_SPCReadPortFunction( nullptr )
, m_DSPWriteFunction( nullptr )
, m_DSPReadFunction( nullptr )
, m_ConvertRuntimeAddressFunction( nullptr )
, m_UpdateInstructionOutput( nullptr )
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
	m_Rom.removeFromParent();
	m_DynamicLoad8.removeFromParent();
	m_DynamicLoad16.removeFromParent();
}

void Recompiler::CreateFunctions()
{
	auto voidFunctionType = llvm::FunctionType::get( llvm::Type::getVoidTy( m_LLVMContext ), llvm::Type::getVoidTy( m_LLVMContext ), false );
	for ( const auto& functionName : m_FunctionNames )
	{
		auto function = llvm::Function::Create( voidFunctionType, llvm::Function::ExternalLinkage, functionName, m_RecompilationModule );
		m_Functions.emplace( functionName, function );
	}
}

void Recompiler::AddInstructionStringGlobalVariables()
{
	struct AddInstructionStringGlobalVariablesVisitor
	{
		AddInstructionStringGlobalVariablesVisitor( Recompiler& recompiler ) : m_Recompiler( recompiler ) {}

		void operator()( const Label& label )
		{
		}

		void operator()( const Instruction& instruction )
		{
			m_Recompiler.AddOffsetToInstructionString( instruction.GetOffset(), instruction.GetInstructionString() );
		}

		Recompiler& m_Recompiler;
	};

	AddInstructionStringGlobalVariablesVisitor addInstructionStringGlobalVariablesVisitor( *this );
	for ( auto&& n : m_Program )
	{
		std::visit( addInstructionStringGlobalVariablesVisitor, n );
	}
}

// Visit all the Label nodes and set up the basic blocks:
void Recompiler::InitialiseBasicBlocksFromLabelNames()
{
	struct InitialiseBasicBlocksFromLabelsVisitor
	{
		InitialiseBasicBlocksFromLabelsVisitor( Recompiler& recompiler, llvm::LLVMContext& context ) : m_Recompiler( recompiler ), m_Context( context ) {}
		
		void operator()( const Label& label )
		{
			const auto& labelName = label.GetName();
			const auto labelOffset = label.GetOffset();
			const auto& functions = m_Recompiler.GetFunctions();
			const auto& labelsToFunctions = m_Recompiler.GetLabelsToFunctions();
			const auto& functionInfo = labelsToFunctions.find( labelOffset );
			if ( functionInfo != labelsToFunctions.end() )
			{
				for ( const auto& functionEntry : functionInfo->second )
				{
					const auto functionFind = functions.find( functionEntry.first );
					if ( functionFind != functions.end() )
					{
						auto function = functionFind->second;
						assert( function );
						const auto basicBlockName = functionEntry.first + "_" + labelName;
						auto basicBlock = llvm::BasicBlock::Create( m_Context, basicBlockName, function );
						m_Recompiler.AddLabelNameToBasicBlock( basicBlockName, basicBlock );

						const bool entryPoint = functionEntry.second;
						if ( entryPoint )
						{
							basicBlock->moveBefore( &(function->getEntryBlock()) );
						}
					}
				}
			}
		}

		void operator()( const Instruction& )
		{
		}

		Recompiler& m_Recompiler;
		llvm::LLVMContext& m_Context;
	};

	InitialiseBasicBlocksFromLabelsVisitor initialiseBasicBlocksFromLabelsVisitor( *this, m_LLVMContext );
	for ( auto&& n : m_Program )
	{
		std::visit( initialiseBasicBlocksFromLabelsVisitor, n );
	}
}

void Recompiler::GenerateCodeForInstruction( const Instruction& instruction, const std::string& functionName )
{
	PerformUpdateInstructionOutput( instruction.GetOffset(), instruction.GetInstructionString() );
	auto nextPC = ComputeNewPC( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetTotalSize() ), false ) ) );
	switch ( instruction.GetOpcode() )
	{
	case 0x09: // ORA immediate
	{
		if ( instruction.GetMemoryMode() == SIXTEEN_BIT )
		{
			auto operandValue = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
			PerformOra16( operandValue );
		}
		else
		{
			auto operandValue = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
			PerformOra8( operandValue );
		}
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x29: // AND immediate
	{
		if ( instruction.GetMemoryMode() == SIXTEEN_BIT )
		{
			auto operandValue = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
			PerformAnd16( operandValue );
		}
		else
		{
			auto operandValue = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
			PerformAnd8( operandValue );
		}
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x49: // EOR immediate
	{
		if ( instruction.GetMemoryMode() == SIXTEEN_BIT )
		{
			auto operandValue = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
			PerformEor16( operandValue );
		}
		else
		{
			auto operandValue = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
			PerformEor8( operandValue );
		}
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xA9: // LDA immediate
	{
		if ( instruction.GetMemoryMode() == SIXTEEN_BIT )
		{
			auto operandValue = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
			PerformLda16( operandValue );
		}
		else
		{
			auto operandValue = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
			PerformLda8( operandValue );
		}
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xA2: // LDX immediate
	{
		if ( instruction.GetIndexMode() == SIXTEEN_BIT )
		{
			auto operandValue = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
			PerformLdx16( operandValue );
		}
		else
		{
			auto operandValue = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
			PerformLdx8( operandValue );
		}
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xA0: // LDY immediate
	{
		if ( instruction.GetIndexMode() == SIXTEEN_BIT )
		{
			auto operandValue = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
			PerformLdy16( operandValue );
		}
		else
		{
			auto operandValue = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
			PerformLdy8( operandValue );
		}
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xC9: // CMP immediate
	{
		if ( instruction.GetMemoryMode() == SIXTEEN_BIT )
		{
			auto lValue = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
			auto rValue = CreateLoadA16();
			PerformCmp16( lValue, rValue );
		}
		else
		{
			auto lValue = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
			auto rValue = CreateLoadA8();
			PerformCmp8( lValue, rValue );
		}
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xE0: // CPX immediate
	{
		if ( instruction.GetIndexMode() == SIXTEEN_BIT )
		{
			auto lValue = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
			auto rValue = CreateLoadX16();
			PerformCmp16( lValue, rValue );
		}
		else
		{
			auto lValue = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
			auto rValue = CreateLoadX8();
			PerformCmp8( lValue, rValue );
		}
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xC0: // CPY immediate
	{
		if ( instruction.GetIndexMode() == SIXTEEN_BIT )
		{
			auto lValue = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
			auto rValue = CreateLoadY16();
			PerformCmp16( lValue, rValue );
		}
		else
		{
			auto lValue = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
			auto rValue = CreateLoadY8();
			PerformCmp8( lValue, rValue );
		}
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x18: // CLC implied
	{
		ClearCarry();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x38: // SEC implied
	{
		SetCarry();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xD8: // CLD implied
	{
		ClearDecimal();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x58: // CLI implied
	{
		ClearInterrupt();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xB8: // CLV implied
	{
		ClearOverflow();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xF8: // SED implied
	{
		SetDecimal();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x78: // SEI implied
	{
		SetInterrupt();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xEB: // XBA
	{
		PerformXba();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x1B: // TCS
	{
		PerformTcs();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x5B: // TCD
	{
		PerformTcd();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x7B: // TDC
	{
		PerformTdc();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x3B: // TSC
	{
		PerformTsc();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x60: // RTS
	{
		PerformRts();
	}
	break;
	case 0x6B: // RTL
	{
		PerformRtl();
	}
	break;
	case 0x40: // RTI
	{
		PerformRti();
	}
	break;
	case 0x80: // BRA
	{
		auto pc = ComputeNewPC( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( static_cast<int8_t>( instruction.GetTotalSize() ) + static_cast<int8_t>( instruction.GetOperand() ) ), true ) ) );
		PerformBra( instruction.GetJumpLabelName(), functionName, pc );
	}
	break;
	case 0x4C: // JMP abs
	{
		PerformJmpAbs( instruction.GetJumpLabelName(), functionName, instruction.GetOperand() );
	}
	break;
	case 0x5C: // JMP long
	{
		PerformJmpLng( instruction.GetJumpLabelName(),functionName, instruction.GetOperand() );
	}
	break;
	case 0xF4: // PEA
	{
		auto value = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), false ) );
		PerformPea( value );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xC2: // REP
	{
		auto value = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( instruction.GetOperand() ), false ) );
		PerformRep( value );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xE2: // SEP
	{
		auto value = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( instruction.GetOperand() ), false ) );
		PerformSep( value );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x20: // JSR absolute
	{
		PerformJsr( instruction.GetOffset(), instruction.GetOperand() );
	}
	break;
	case 0xFC: // JSR absolute Idx X
	{
		PerformJsrAbsIdxX( instruction.GetOffset(), instruction.GetOperand() );
	}
	break;
	case 0x7C: // JMP absolute Idx X
	{
		PerformJmpAbsIdxX( instruction.GetOffset(), functionName, instruction.GetOperand() );
	}
	break;
	case 0x22: // JSL long
	{
		PerformJsl( instruction.GetOffset(), instruction.GetOperand() );
	}
	break;
	case 0x90: // BCC
	{
		auto pcBranchTaken = ComputeNewPC( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( static_cast<int8_t>( instruction.GetTotalSize() ) + static_cast<int8_t>( instruction.GetOperand() ) ), true ) ) );
		auto pcBranchNotTaken = ComputeNewPC( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetTotalSize() ), false ) ) );
		PerformBcc( instruction.GetJumpLabelName(), functionName, pcBranchTaken, pcBranchNotTaken );
	}
	break;
	case 0xB0: // BCS
	{
		auto pcBranchTaken = ComputeNewPC( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( static_cast<int8_t>( instruction.GetTotalSize() ) + static_cast<int8_t>( instruction.GetOperand() ) ), true ) ) );
		auto pcBranchNotTaken = ComputeNewPC( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetTotalSize() ), false ) ) );
		PerformBcs( instruction.GetJumpLabelName(), functionName, pcBranchTaken, pcBranchNotTaken );
	}
	break;
	case 0xF0: // BEQ
	{
		auto pcBranchTaken = ComputeNewPC( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( static_cast<int8_t>( instruction.GetTotalSize() ) + static_cast<int8_t>( instruction.GetOperand() ) ), true ) ) );
		auto pcBranchNotTaken = ComputeNewPC( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetTotalSize() ), false ) ) );
		PerformBeq( instruction.GetJumpLabelName(), functionName, pcBranchTaken, pcBranchNotTaken );
	}
	break;
	case 0xD0: // BNE
	{
		auto pcBranchTaken = ComputeNewPC( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( static_cast<int8_t>( instruction.GetTotalSize() ) + static_cast<int8_t>( instruction.GetOperand() ) ), true ) ) );
		auto pcBranchNotTaken = ComputeNewPC( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetTotalSize() ), false ) ) );
		PerformBne( instruction.GetJumpLabelName(), functionName, pcBranchTaken, pcBranchNotTaken );
	}
	break;
	case 0x30: // BMI
	{
		auto pcBranchTaken = ComputeNewPC( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( static_cast<int8_t>( instruction.GetTotalSize() ) + static_cast<int8_t>( instruction.GetOperand() ) ), true ) ) );
		auto pcBranchNotTaken = ComputeNewPC( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetTotalSize() ), false ) ) );
		PerformBmi( instruction.GetJumpLabelName(), functionName, pcBranchTaken, pcBranchNotTaken );
	}
	break;
	case 0x10: // BPL
	{
		auto pcBranchTaken = ComputeNewPC( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( static_cast<int8_t>( instruction.GetTotalSize() ) + static_cast<int8_t>( instruction.GetOperand() ) ), true ) ) );
		auto pcBranchNotTaken = ComputeNewPC( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetTotalSize() ), false ) ) );
		PerformBpl( instruction.GetJumpLabelName(), functionName, pcBranchTaken, pcBranchNotTaken );
	}
	break;
	case 0x50: // BVC
	{
		auto pcBranchTaken = ComputeNewPC( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( static_cast<int8_t>( instruction.GetTotalSize() ) + static_cast<int8_t>( instruction.GetOperand() ) ), true ) ) );
		auto pcBranchNotTaken = ComputeNewPC( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetTotalSize() ), false ) ) );
		PerformBvc( instruction.GetJumpLabelName(), functionName, pcBranchTaken, pcBranchNotTaken );
	}
	break;
	case 0x70: // BVS
	{
		auto pcBranchTaken = ComputeNewPC( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( static_cast<int8_t>( instruction.GetTotalSize() ) + static_cast<int8_t>( instruction.GetOperand() ) ), true ) ) );
		auto pcBranchNotTaken = ComputeNewPC( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetTotalSize() ), false ) ) );
		PerformBvs( instruction.GetJumpLabelName(), functionName, pcBranchTaken, pcBranchNotTaken );
	}
	break;
	case 0x48: // PHA
	{
		PerformPha();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xDA: // PHX
	{
		PerformPhx();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x5A: // PHY
	{
		PerformPhy();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x8B: // PHB
	{
		PerformPhb();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x0B: // PHD
	{
		PerformPhd();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x4B: // PHK
	{
		PerformPhk();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x08: // PHP
	{
		PerformPhp();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x68: // PLA
	{
		PerformPla();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xFA: // PLX
	{
		PerformPlx();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x7A: // PLY
	{
		PerformPly();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xAB: // PLB
	{
		PerformPlb();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x2B: // PLD
	{
		PerformPld();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x28: // PLP
	{
		PerformPlp();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x3A: // DEC accumulator
	{
		PerformDec();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xCE: // DEC Abs
	{
		PerformDecAbs( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xC6: // DEC Dir
	{
		PerformDecDir( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x1A: // INC accumulator
	{
		PerformInc();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xE6: // INC Dir
	{
		PerformIncDir( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xEE: // INC Abs
	{
		PerformIncAbs( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xCA: // DEX accumulator
	{
		PerformDex();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xE8: // INX accumulator
	{
		PerformInx();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x88: // DEY accumulator
	{
		PerformDey();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xC8: // INY accumulator
	{
		PerformIny();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x0A: // ASL accumulator
	{
		PerformAsl();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x0E: // ASL Abs
	{
		PerformAslAbs( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x06: // ASL Dir
	{
		PerformAslDir( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x4A: // LSR accumulator
	{
		PerformLsr();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x4E: // LSR Abs
	{
		PerformLsrAbs( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x46: // LSR Dir
	{
		PerformLsrDir( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x2A: // ROL accumulator
	{
		PerformRol();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x2E: // ROL Abs
	{
		PerformRolAbs( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x26: // ROL Dir
	{
		PerformRolDir( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x6A: // ROR accumulator
	{
		PerformRor();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x6E: // ROR Abs
	{
		PerformRorAbs( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x66: // ROR Abs
	{
		PerformRorDir( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xAA: // TAX
	{
		PerformTax();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xA8: // TAY
	{
		PerformTay();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xBA: // TSX
	{
		PerformTsx();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x8A: // TXA
	{
		PerformTxa();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x9A: // TXS
	{
		PerformTxs();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x9B: // TXY
	{
		PerformTxy();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x98: // TYA
	{
		PerformTya();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xBB: // TYX
	{
		PerformTyx();
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x69: // ADC
	{
		if ( instruction.GetMemoryMode() == SIXTEEN_BIT )
		{
			auto data = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
			PerformAdc16( data );
		}
		else
		{
			auto data = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
			PerformAdc8( data );
		}
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xE9: // SBC
	{
		if ( instruction.GetMemoryMode() == SIXTEEN_BIT )
		{
			auto data = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
			PerformSbc16( data );
		}
		else
		{
			auto data = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
			PerformSbc8( data );
		}
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x89: // BIT
		if ( instruction.GetMemoryMode() == SIXTEEN_BIT )
		{
			auto data = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
			PerformBit16Imm( data );
		}
		else
		{
			auto data = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
			PerformBit8Imm( data );
		}
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
		break;
	case 0xAF: // LDA Long
	{
		PerformLdaLong( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x8F: // STA Long
	{
		PerformStaLong( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xAD: // LDA Abs
	{
		PerformLdaAbs( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x8D: // STA Abs
	{
		PerformStaAbs( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x8E: // STX Abs
	{
		PerformStxAbs( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x8C: // STY Abs
	{
		PerformStyAbs( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x9C: // STZ Abs
	{
		PerformStzAbs( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xAE: // LDX Abs
	{
		PerformLdxAbs( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xAC: // LDY Abs
	{
		PerformLdyAbs( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x1C: // TRB Abs
	{
		PerformTrbAbs( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x14: // TRB Dir
	{
		PerformTrbDir( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x0C: // TSB Abs
	{
		PerformTsbAbs( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x04: // TSB Dir
	{
		PerformTsbDir( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x2C: // BIT Abs
	{
		PerformBitAbs( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x2D: // AND Abs
	{
		PerformAndAbs( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x25: // AND Dir
	{
		PerformAndDir( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x4D: // EOR Abs
	{
		PerformEorAbs( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x55: // EOR Dir IdxX
	{
		PerformEorDirIdxX( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x45: // EOR Dir
	{
		PerformEorDir( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x0D: // ORA Abs
	{
		PerformOraAbs( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x05: // ORA Dir
	{
		PerformOraDir( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x19: // ORA Abs IdxY
	{
		PerformOraAbsIdxY( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xCD: // CMP Abs
	{
		PerformCmpAbs( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xC5: // CMP Dir
	{
		PerformCmpDir( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xEC: // CPX Abs
	{
		PerformCpxAbs( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xE4: // CPX Dir
	{
		PerformCpxDir( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xCC: // CPY Abs
	{
		PerformCpyAbs( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xC4: // CPY Dir
	{
		PerformCpyDir( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xA5: // LDA Dir
	{
		PerformLdaDir( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xA6: // LDX Dir
	{
		PerformLdxDir( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xA4: // LDY Dir
	{
		PerformLdyDir( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x85: // STA dir
	{
		PerformStaDir( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x86: // STX dir
	{
		PerformStxDir( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x84: // STY dir
	{
		PerformStyDir( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x64: // STZ dir
	{
		PerformStzDir( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x54: // MVN
	{
		PerformMvn( instruction.GetOperand(), instruction.GetOffset(), instruction.GetInstructionString() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x44: // MVP
	{
		PerformMvn( instruction.GetOperand(), instruction.GetOffset(), instruction.GetInstructionString() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x24: // BIT Dir
	{
		PerformBitDir( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xB5: // LDA DirIdxX
	{
		PerformLdaDirIdxX( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xB4: // LDY DirIdxX
	{
		PerformLdyDirIdxX( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x95: // STA DirIdxX
	{
		PerformStaDirIdxX( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x94: // STY DirIdxX
	{
		PerformStyDirIdxX( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x74: // STZ DirIdxX
	{
		PerformStzDirIdxX( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xB6: // LDX DirIdxY
	{
		PerformLdxDirIdxY( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x96: // STX DirIdxY
	{
		PerformStxDirIdxY( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xBF: // LDA LongIdxX
	{
		PerformLdaLongIdxX( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x9F: // STA LongIdxX
	{
		PerformStaLongIdxX( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xBD: // LDA AbsIdxX
	{
		PerformLdaAbsIdxX( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xB9: // LDX AbsIdxY
	{
		PerformLdaAbsIdxY( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xBC: // LDY AbsIdxX
	{
		PerformLdyAbsIdxX( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x9D: // STA AbsIdxX
	{
		PerformStaAbsIdxX( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x9E: // STZ AbsIdxX
	{
		PerformStzAbsIdxX( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xBE: // LDX AbsIdxY
	{
		PerformLdxAbsIdxY( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x99: // STA AbsIdxY
	{
		PerformStaAbsIdxY( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x65: // ADC Dir
	{
		PerformAdcDir( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xF5: // SBC Dir Idx X
	{
		PerformSbcDirIdxX( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x6D: // ADC Abs
	{
		PerformAdcAbs( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x7D: // ADC Abs Idx X
	{
		PerformAdcAbsIdxX( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xED: // SBC Abs
	{
		PerformSbcAbs( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xFD: // SBC Abs Idx X
	{
		PerformSbcAbsIdxX( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x6F: // ADC Long
	{
		PerformAdcLong( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0x7F: // ADC Long Idx X
	{
		PerformAdcLongIdxX( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xFF: // SBC Long Idx X
	{
		PerformSbcLongIdxX( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xE5: // SBC Dir
	{
		PerformSbcDir( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xB7: // LDA [DIRECT],Y
	{
		PerformLdaDirIndLngIdxY( instruction.GetOperand() );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	case 0xEA: // NOP
	{
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC );
	}
	break;
	default:
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), nextPC, false );
		break;
	}
}

void Recompiler::GenerateCode()
{
	const auto numProgramNodes = m_Program.size();
	for ( size_t nodeIndex = 0; nodeIndex < numProgramNodes; nodeIndex++ )
	{
		const auto& node = m_Program[ nodeIndex ];
		if ( std::holds_alternative<Label>( node ) )
		{
			const auto& label = std::get<Label>( node );
			const auto& labelName = label.GetName();
			const auto labelOffset = label.GetOffset();
			const auto& functionInfo = m_LabelsToFunctions.find( labelOffset );
			if ( functionInfo != m_LabelsToFunctions.end() )
			{
				for ( const auto& functionEntry : functionInfo->second )
				{
					const auto basicBlockName = functionEntry.first + "_" + labelName;
					auto search = m_LabelNamesToBasicBlocks.find( basicBlockName );
					assert( search != m_LabelNamesToBasicBlocks.end() );

					auto basicBlock = search->second;

					m_CurrentBasicBlock = basicBlock;
					m_IRBuilder.SetInsertPoint( basicBlock );

					auto codeGenIndex = nodeIndex + 1;
					while ( std::holds_alternative<Instruction>( m_Program[ codeGenIndex ] ) )
					{
						assert( std::holds_alternative<Instruction>( m_Program[ codeGenIndex ] ) );
						const auto& instruction = std::get<Instruction>( m_Program[ codeGenIndex ] );
						GenerateCodeForInstruction( instruction, functionEntry.first );

						codeGenIndex++;
					}
				}
			}
		}
	}
}

void Recompiler::Recompile()
{
	llvm::InitializeNativeTarget();

	// Add cycle function that will called every time an instruction is executed:
	m_CycleFunction = llvm::Function::Create( llvm::FunctionType::get( llvm::Type::getVoidTy( m_LLVMContext ), { llvm::Type::getInt32Ty( m_LLVMContext ), llvm::Type::getInt32Ty( m_LLVMContext )}, false ), llvm::Function::ExternalLinkage, "romCycle", m_RecompilationModule );
	m_ConvertRuntimeAddressFunction = llvm::Function::Create( llvm::FunctionType::get( llvm::Type::getInt32Ty( m_LLVMContext ), llvm::Type::getInt32Ty( m_LLVMContext ), false ), llvm::Function::ExternalLinkage, "convertRuntimeAddressToOffset", m_RecompilationModule );
	m_UpdateInstructionOutput = llvm::Function::Create( llvm::FunctionType::get( llvm::Type::getVoidTy( m_LLVMContext ), { llvm::Type::getInt32Ty( m_LLVMContext ), llvm::Type::getInt8PtrTy( m_LLVMContext ) }, false ), llvm::Function::ExternalLinkage, "updateInstructionOutput", m_RecompilationModule );
	m_PanicFunction = llvm::Function::Create( llvm::FunctionType::get( llvm::Type::getVoidTy( m_LLVMContext ), false ), llvm::Function::ExternalLinkage, "panic", m_RecompilationModule );

	m_SPCWritePortFunction = llvm::Function::Create( llvm::FunctionType::get( llvm::Type::getVoidTy( m_LLVMContext ), { llvm::Type::getInt32Ty( m_LLVMContext ), llvm::Type::getInt32Ty( m_LLVMContext ) }, false ), llvm::Function::ExternalLinkage, "spcWritePort", m_RecompilationModule );
	m_SPCReadPortFunction = llvm::Function::Create( llvm::FunctionType::get( llvm::Type::getInt32Ty( m_LLVMContext ), { llvm::Type::getInt32Ty( m_LLVMContext ) }, false ), llvm::Function::ExternalLinkage, "spcReadPort", m_RecompilationModule );

	m_DSPWriteFunction = llvm::Function::Create( llvm::FunctionType::get( llvm::Type::getVoidTy( m_LLVMContext ), { llvm::Type::getInt32Ty( m_LLVMContext ), llvm::Type::getInt8Ty( m_LLVMContext ) }, false ), llvm::Function::ExternalLinkage, "dspWrite", m_RecompilationModule );
	m_DSPReadFunction = llvm::Function::Create( llvm::FunctionType::get( llvm::Type::getInt8Ty( m_LLVMContext ), { llvm::Type::getInt32Ty( m_LLVMContext ) }, false ), llvm::Function::ExternalLinkage, "dspRead", m_RecompilationModule );

	auto mainFunctionType = llvm::FunctionType::get( llvm::Type::getVoidTy( m_LLVMContext ), llvm::Type::getVoidTy( m_LLVMContext ), false );
	m_MainFunction = llvm::Function::Create( mainFunctionType, llvm::Function::ExternalLinkage, "start", m_RecompilationModule );

	auto entry = llvm::BasicBlock::Create( m_LLVMContext, "EntryBlock", m_MainFunction );
	m_PanicBlock = llvm::BasicBlock::Create( m_LLVMContext, "PanicBlock", m_MainFunction );
	m_IRBuilder.SetInsertPoint( m_PanicBlock );
	m_IRBuilder.CreateCall( m_PanicFunction );
	m_IRBuilder.CreateRetVoid();
	m_IRBuilder.SetInsertPoint( entry );

	AddInstructionStringGlobalVariables();
	CreateFunctions();
	InitialiseBasicBlocksFromLabelNames();
	GenerateCode();

	SelectBlock( entry );
	m_IRBuilder.CreateStore( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, m_RomResetAddr, true ) ), &m_registerPC );
	
	auto resetFunction = m_Functions[ m_RomResetFuncName ];
	m_IRBuilder.CreateCall( resetFunction );
	llvm::verifyModule( m_RecompilationModule, &llvm::errs() );

	std::error_code EC;
	llvm::raw_fd_ostream outputHumanReadable( "smk.ll", EC );
	m_RecompilationModule.print( outputHumanReadable, nullptr );
}

auto Recompiler::CreateRegisterWidthTest( const uint64_t flag )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, flag, true ) ), "" );
	auto cond = m_IRBuilder.CreateICmpEQ( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x0, true ) ) );
	auto thenBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto elseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto endBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	if ( m_CurrentBasicBlock )
	{
		thenBlock->moveAfter( m_CurrentBasicBlock );
		elseBlock->moveAfter( thenBlock );
		endBlock->moveAfter( elseBlock );
	}

	return std::make_tuple( cond, thenBlock, elseBlock, endBlock );
}

void Recompiler::AddLabelNameToBasicBlock( const std::string& labelName, llvm::BasicBlock* basicBlock )
{
	m_LabelNamesToBasicBlocks.emplace( labelName, basicBlock );
}

void Recompiler::AddOffsetToInstructionString( const uint32_t offset, const std::string& instructionString )
{
	auto s = m_IRBuilder.CreateGlobalString( instructionString, "" );
	m_OffsetsToInstructionStringGlobalVariable.emplace( offset, s );
}

llvm::BasicBlock* Recompiler::CreateBlock( const char* name )
{
	auto block = llvm::BasicBlock::Create( m_LLVMContext, name );
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
	m_IRBuilder.CreateCall( m_PanicFunction );
}

void Recompiler::PerformRomCycle( llvm::Value* value, const bool implemented )
{
	std::vector<llvm::Value*> params = { value, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( implemented ? 1 : 0 ), false ) ) };
	m_IRBuilder.CreateCall( m_CycleFunction, params, "" );
}

void Recompiler::PerformRomCycle( llvm::Value* value, llvm::Value* newPc, const bool implemented )
{
	m_IRBuilder.CreateStore( newPc, &m_registerPC );

	std::vector<llvm::Value*> params = { value, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( implemented ? 1 : 0 ), false ) ) };
	m_IRBuilder.CreateCall( m_CycleFunction, params, "" );
}

void Recompiler::PerformUpdateInstructionOutput( const uint32_t offset, const std::string& instructionString )
{
	auto pc32 = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerPC, "" ), llvm::Type::getInt32Ty( m_LLVMContext ), "" );
	auto pb32 = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerPB, "" ), llvm::Type::getInt32Ty( m_LLVMContext ), "" );
	auto pb32Shifted = m_IRBuilder.CreateShl( pb32, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 16 ), false ) ), "" );
	auto finalRuntimePC = m_IRBuilder.CreateOr( pc32, pb32Shifted, "" );

	auto s = m_OffsetsToInstructionStringGlobalVariable[ offset ];
	std::vector<llvm::Value*> params = { finalRuntimePC, m_IRBuilder.CreateConstGEP2_32( s->getValueType(), s, 0, 0, "" ) };
	m_IRBuilder.CreateCall( m_UpdateInstructionOutput, params, "" );
}


void Recompiler::PerformSep( llvm::Value* value )
{
	auto newP = m_IRBuilder.CreateOr( m_IRBuilder.CreateLoad( &m_registerP, "" ), value, "" );

	auto result = m_IRBuilder.CreateAnd( newP, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x10, true ) ), "" );
	auto cond = m_IRBuilder.CreateICmpNE( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x0, true ) ) );

	auto thenBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto endBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	if ( m_CurrentBasicBlock )
	{
		thenBlock->moveAfter( m_CurrentBasicBlock );
		endBlock->moveAfter( thenBlock );
	}

	m_IRBuilder.CreateCondBr( cond, thenBlock, endBlock );
	SelectBlock( thenBlock );
	
	auto x = m_IRBuilder.CreateBitCast( &m_registerX, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" );
	auto xHigh8Ptr = m_IRBuilder.CreateInBoundsGEP( llvm::Type::getInt8Ty( m_LLVMContext ), x, llvm::ConstantInt::get( llvm::Type::getInt32Ty( m_LLVMContext ), 1 ) );
	m_IRBuilder.CreateStore( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0, true ) ), xHigh8Ptr );

	auto y = m_IRBuilder.CreateBitCast( &m_registerY, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" );
	auto yHigh8Ptr = m_IRBuilder.CreateInBoundsGEP( llvm::Type::getInt8Ty( m_LLVMContext ), y, llvm::ConstantInt::get( llvm::Type::getInt32Ty( m_LLVMContext ), 1 ) );
	m_IRBuilder.CreateStore( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0, true ) ), yHigh8Ptr );

	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
	
	m_IRBuilder.CreateStore( newP, &m_registerP );
}

void Recompiler::PerformRep( llvm::Value* value )
{
	auto complement = m_IRBuilder.CreateXor( value, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0xff, true ) ), "" );
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), complement, "" );
	m_IRBuilder.CreateStore( result, &m_registerP );
}

void Recompiler::PerformBvc( const std::string& labelName, const std::string& functionName, llvm::Value* pcBranchTaken, llvm::Value* pcBranchNotTaken )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x40, true ) ), "" );
	auto cond = m_IRBuilder.CreateICmpNE( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x40, true ) ) );
	
	AddConditionalBranch( cond, labelName, functionName, pcBranchTaken, pcBranchNotTaken );
}

void Recompiler::PerformBvs( const std::string& labelName, const std::string& functionName, llvm::Value* pcBranchTaken, llvm::Value* pcBranchNotTaken )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x40, true ) ), "" );
	auto cond = m_IRBuilder.CreateICmpEQ( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x40, true ) ) );
	
	AddConditionalBranch( cond, labelName, functionName, pcBranchTaken, pcBranchNotTaken );
}

void Recompiler::PerformBpl( const std::string& labelName, const std::string& functionName, llvm::Value* pcBranchTaken, llvm::Value* pcBranchNotTaken )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x80, true ) ), "" );
	auto cond = m_IRBuilder.CreateICmpNE( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x80, true ) ) );
	
	AddConditionalBranch( cond, labelName, functionName, pcBranchTaken, pcBranchNotTaken );
}

void Recompiler::PerformBmi( const std::string& labelName, const std::string& functionName, llvm::Value* pcBranchTaken, llvm::Value* pcBranchNotTaken )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x80, true ) ), "" );
	auto cond = m_IRBuilder.CreateICmpEQ( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x80, true ) ) );
	
	AddConditionalBranch( cond, labelName, functionName, pcBranchTaken, pcBranchNotTaken );
}

void Recompiler::PerformBne( const std::string& labelName, const std::string& functionName, llvm::Value* pcBranchTaken, llvm::Value* pcBranchNotTaken )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x2, true ) ), "" );
	auto cond = m_IRBuilder.CreateICmpNE( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x2, true ) ) );
	
	AddConditionalBranch( cond, labelName, functionName, pcBranchTaken, pcBranchNotTaken );
}

void Recompiler::PerformBeq( const std::string& labelName, const std::string& functionName, llvm::Value* pcBranchTaken, llvm::Value* pcBranchNotTaken )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x2, true ) ), "" );
	auto cond = m_IRBuilder.CreateICmpEQ( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x2, true ) ) );
	
	AddConditionalBranch( cond, labelName, functionName, pcBranchTaken, pcBranchNotTaken );
}

void Recompiler::PerformBcs( const std::string& labelName, const std::string& functionName, llvm::Value* pcBranchTaken, llvm::Value* pcBranchNotTaken )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x1, true ) ), "" );
	auto cond = m_IRBuilder.CreateICmpEQ( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x1, true ) ) );

	AddConditionalBranch( cond, labelName, functionName, pcBranchTaken, pcBranchNotTaken );
}

void Recompiler::PerformBcc( const std::string& labelName, const std::string& functionName, llvm::Value* pcBranchTaken, llvm::Value* pcBranchNotTaken )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x1, true ) ), "" );
	auto cond = m_IRBuilder.CreateICmpNE( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x1, true ) ) );
	
	AddConditionalBranch( cond, labelName, functionName, pcBranchTaken, pcBranchNotTaken );
}

void Recompiler::AddConditionalBranch( llvm::Value* cond, const std::string& labelName, const std::string& functionName, llvm::Value* pcBranchTaken, llvm::Value* pcBranchNotTaken )
{
	auto search = m_LabelNamesToBasicBlocks.find( functionName + "_" + labelName );
	if ( search != m_LabelNamesToBasicBlocks.end() )
	{
		auto takeBranchBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
		if ( m_CurrentBasicBlock )
		{
			takeBranchBlock->moveAfter( m_CurrentBasicBlock );
		}
		auto elseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
		if ( takeBranchBlock )
		{
			elseBlock->moveAfter( takeBranchBlock );
		}
		m_IRBuilder.CreateCondBr( cond, takeBranchBlock, elseBlock );
		SelectBlock( takeBranchBlock );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), pcBranchTaken );
		m_IRBuilder.CreateBr( search->second );
		SelectBlock( elseBlock );
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), pcBranchNotTaken );
	}
	else
	{
		m_IRBuilder.CreateBr( m_PanicBlock );
	}
}

void Recompiler::PerformJsl( const uint32_t instructionOffset, const uint32_t jumpAddress )
{
	PushByteToStack( m_IRBuilder.CreateLoad( &m_registerPB, "" ) );
	auto pcPlus3 = m_IRBuilder.CreateAdd( m_IRBuilder.CreateLoad( &m_registerPC, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 3, false ) ), "" );
	PushWordToStack( pcPlus3 );

	const uint16_t pc = jumpAddress & 0xffff;
	const uint8_t pb = ( jumpAddress & 0xff0000 ) >> 16;
	m_IRBuilder.CreateStore( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( pc ), false ) ), &m_registerPC );
	m_IRBuilder.CreateStore( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( pb ), false ) ), &m_registerPB );

	auto findFunctionNameResult = m_OffsetToFunctionName.find( instructionOffset );
	assert( findFunctionNameResult != m_OffsetToFunctionName.end() );
	
	const auto& functionName = findFunctionNameResult->second;
	auto findFunctionResult = m_Functions.find( functionName );
	assert( findFunctionResult != m_Functions.end() );
	auto function = findFunctionResult->second;
	m_IRBuilder.CreateCall( function );

	// TODO - check we need to reset the current basic block and if so work out why?
	m_CurrentBasicBlock = nullptr;
}

void Recompiler::PerformJsr( const uint32_t instructionOffset, const uint32_t jumpAddress )
{
	auto pcPlus2 = m_IRBuilder.CreateAdd( m_IRBuilder.CreateLoad( &m_registerPC, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 2, false ) ), "" );
	PushWordToStack( pcPlus2  );

	const uint16_t pc = jumpAddress & 0xffff;
	m_IRBuilder.CreateStore( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( pc ), false ) ), &m_registerPC );

	auto findFunctionNameResult = m_OffsetToFunctionName.find( instructionOffset );
	assert( findFunctionNameResult != m_OffsetToFunctionName.end() );

	const auto& functionName = findFunctionNameResult->second;
	auto findFunctionResult = m_Functions.find( functionName );
	assert( findFunctionResult != m_Functions.end() );
	auto function = findFunctionResult->second;
	m_IRBuilder.CreateCall( function );

	m_CurrentBasicBlock = nullptr;
}

void Recompiler::PerformJsrAbsIdxX( const uint32_t instructionOffset, const uint32_t jumpAddress )
{
	auto pcPlus2 = m_IRBuilder.CreateAdd( m_IRBuilder.CreateLoad( &m_registerPC, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 2, false ) ), "" );
	PushWordToStack( pcPlus2 );

	auto pb = m_IRBuilder.CreateShl( m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerPB ), llvm::Type::getInt32Ty( m_LLVMContext ) ), 16 );
	auto operandAddress = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( jumpAddress & 0xffff ), false ) );
	auto ptrLowAddress = m_IRBuilder.CreateAdd( operandAddress, m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerX ), llvm::Type::getInt32Ty( m_LLVMContext ) ) );
	auto ptrHighAddress = m_IRBuilder.CreateAdd( ptrLowAddress, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 1 ), false ) ) );
	auto finalPtrLowAddress = m_IRBuilder.CreateOr( pb, ptrLowAddress );
	auto finalPtrHighAddress = m_IRBuilder.CreateOr( pb, ptrHighAddress );
	auto ptrLow = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( DynamicLoad8( finalPtrLowAddress ) ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto ptrHigh = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( DynamicLoad8( finalPtrHighAddress ) ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto finalJumpAddress = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( ptrHigh, 8 ), ptrLow );
	auto newPC = m_IRBuilder.CreateTrunc( m_IRBuilder.CreateAnd( finalJumpAddress, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0xffff ), false ) ) ), llvm::Type::getInt16Ty( m_LLVMContext ) );
	m_IRBuilder.CreateStore( newPC, &m_registerPC );
	
	PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );

	auto findJumpTableEntries = m_JumpTables.find( instructionOffset );
	assert( findJumpTableEntries != m_JumpTables.end() );
	
	auto jumpTableEntries = findJumpTableEntries->second;
	const auto numSwitchCases = jumpTableEntries.size();
	auto endBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto sw = m_IRBuilder.CreateSwitch( CreateLoadX16(), endBlock, numSwitchCases );
	for ( const auto& jumpTableEntry : jumpTableEntries )
	{
		const auto& functionName = jumpTableEntry.second;
		auto findFunctionResult = m_Functions.find( functionName );
		assert( findFunctionResult != m_Functions.end() );
		auto function = findFunctionResult->second;

		auto caseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
		SelectBlock( caseBlock );
		m_IRBuilder.CreateCall( function );
		m_IRBuilder.CreateBr( endBlock );
		auto caseValue = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( jumpTableEntry.first ), false ) );
		sw->addCase( caseValue, caseBlock );
	}

	m_CurrentBasicBlock = nullptr;
}

void Recompiler::PerformJmpAbsIdxX( const uint32_t instructionOffset, const std::string& functionName, const uint32_t jumpAddress )
{
	auto pb = m_IRBuilder.CreateShl( m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerPB ), llvm::Type::getInt32Ty( m_LLVMContext ) ), 16 );
	auto operandAddress = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( jumpAddress & 0xffff ), false ) );
	auto ptrLowAddress = m_IRBuilder.CreateAdd( operandAddress, m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerX ), llvm::Type::getInt32Ty( m_LLVMContext ) ) );
	auto ptrHighAddress = m_IRBuilder.CreateAdd( ptrLowAddress, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 1 ), false ) ) );
	auto finalPtrLowAddress = m_IRBuilder.CreateOr( pb, ptrLowAddress );
	auto finalPtrHighAddress = m_IRBuilder.CreateOr( pb, ptrHighAddress );
	auto ptrLow = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( DynamicLoad8( finalPtrLowAddress ) ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto ptrHigh = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( DynamicLoad8( finalPtrHighAddress ) ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto finalJumpAddress = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( ptrHigh, 8 ), ptrLow );
	auto newPC = m_IRBuilder.CreateTrunc( m_IRBuilder.CreateAnd( finalJumpAddress, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0xffff ), false ) ) ), llvm::Type::getInt16Ty( m_LLVMContext ) );
	m_IRBuilder.CreateStore( newPC, &m_registerPC );

	PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );

	auto findJumpTableEntries = m_JumpTables.find( instructionOffset );
	assert( findJumpTableEntries != m_JumpTables.end() );

	auto jumpTableEntries = findJumpTableEntries->second;
	const auto numSwitchCases = jumpTableEntries.size();
	auto endBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto sw = m_IRBuilder.CreateSwitch( CreateLoadX16(), endBlock, numSwitchCases );
	for ( const auto& jumpTableEntry : jumpTableEntries )
	{
		const auto& labelName = functionName + "_" + jumpTableEntry.second;
		auto findLabelResult = m_LabelNamesToBasicBlocks.find( labelName );
		assert( findLabelResult != m_LabelNamesToBasicBlocks.end() );
		auto basicBlock = findLabelResult->second;
		if ( basicBlock == nullptr )
		{
			printf( "debug\n" );
		}

		auto caseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
		SelectBlock( caseBlock );
		m_IRBuilder.CreateBr( basicBlock );
		auto caseValue = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( jumpTableEntry.first ), false ) );
		sw->addCase( caseValue, caseBlock );
	}

	m_CurrentBasicBlock = nullptr;
}

void Recompiler::PerformJmpAbs( const std::string& labelName, const std::string& functionName, const uint32_t jumpAddress )
{
	const uint16_t pc = jumpAddress & 0xffff;
	m_IRBuilder.CreateStore( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( pc ), false ) ), &m_registerPC );

	auto search = m_LabelNamesToBasicBlocks.find( functionName + "_" + labelName );
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

void Recompiler::PerformJmpLng( const std::string& labelName, const std::string& functionName, const uint32_t jumpAddress )
{
	const uint16_t pc = jumpAddress & 0xffff;
	const uint8_t pb = (jumpAddress & 0xff0000) >> 16;
	m_IRBuilder.CreateStore( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( pc ), false ) ), &m_registerPC );
	m_IRBuilder.CreateStore( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( pb ), false ) ), &m_registerPB );

	auto search = m_LabelNamesToBasicBlocks.find( functionName + "_" + labelName );
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

void Recompiler::PerformBra( const std::string& labelName, const std::string& functionName, llvm::Value* newPC )
{
	auto search = m_LabelNamesToBasicBlocks.find( functionName + "_" + labelName );
	if ( search != m_LabelNamesToBasicBlocks.end() )
	{
		PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), newPC );
		m_IRBuilder.CreateBr( search->second );
		m_CurrentBasicBlock = nullptr;
	}
	else
	{
		m_IRBuilder.CreateBr( m_PanicBlock );
	}
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
	m_IRBuilder.CreateRetVoid();
	m_CurrentBasicBlock = nullptr;
}

void Recompiler::PerformRts()
{
	auto addr = PullWordFromStack();
	auto pc = m_IRBuilder.CreateAdd( addr, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 1, false ) ), "" );
	m_IRBuilder.CreateStore( pc, &m_registerPC );
	PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
	m_IRBuilder.CreateRetVoid();
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

void Recompiler::PerformPhy( void )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( X_FLAG );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	PushWordToStack( m_IRBuilder.CreateLoad( &m_registerY ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	PushByteToStack( m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerY, llvm::Type::getInt8PtrTy( m_LLVMContext ) ) ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformPly( void )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( X_FLAG );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto stackValue16 = PullWordFromStack();
	m_IRBuilder.CreateStore( stackValue16, &m_registerY );
	TestAndSetZero16( stackValue16 );
	TestAndSetNegative16( stackValue16 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto stackValue8 = PullByteFromStack();
	m_IRBuilder.CreateStore( stackValue8, m_IRBuilder.CreateBitCast( &m_registerY, llvm::Type::getInt8PtrTy( m_LLVMContext ) ) );
	TestAndSetZero8( stackValue8 );
	TestAndSetNegative8( stackValue8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformPhx( void )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( X_FLAG );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	PushWordToStack( m_IRBuilder.CreateLoad( &m_registerX ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	PushByteToStack( m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerX, llvm::Type::getInt8PtrTy( m_LLVMContext ) ) ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformPlx( void )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( X_FLAG );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto stackValue16 = PullWordFromStack();
	m_IRBuilder.CreateStore( stackValue16, &m_registerX );
	TestAndSetZero16( stackValue16 );
	TestAndSetNegative16( stackValue16 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto stackValue8 = PullByteFromStack();
	m_IRBuilder.CreateStore( stackValue8, m_IRBuilder.CreateBitCast( &m_registerX, llvm::Type::getInt8PtrTy( m_LLVMContext ) ) );
	TestAndSetZero8( stackValue8 );
	TestAndSetNegative8( stackValue8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformPha( void )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	PushWordToStack( m_IRBuilder.CreateLoad( &m_registerA ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	PushByteToStack( m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ) ) ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformPla( void )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto stackValue16 = PullWordFromStack();
	m_IRBuilder.CreateStore( stackValue16, &m_registerA );
	TestAndSetZero16( stackValue16 );
	TestAndSetNegative16( stackValue16 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto stackValue8 = PullByteFromStack();
	m_IRBuilder.CreateStore( stackValue8, m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ) ) );
	TestAndSetZero8( stackValue8 );
	TestAndSetNegative8( stackValue8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
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
	m_IRBuilder.CreateStore( stackValue, &m_registerDB );
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
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( X_FLAG );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	PerformInc16( &m_registerX );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	PerformInc8( m_IRBuilder.CreateBitCast( &m_registerX, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformDex( void )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( X_FLAG );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	PerformDec16( &m_registerX );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	PerformDec8( m_IRBuilder.CreateBitCast( &m_registerX, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformInc( void )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	PerformInc16( &m_registerA );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	PerformInc8( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformIncAbs( const uint32_t address )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), 0x20, "" );
	auto cond = m_IRBuilder.CreateICmpEQ( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x0, true ) ) );
	auto thenBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto elseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto endBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	if ( m_CurrentBasicBlock )
	{
		thenBlock->moveAfter( m_CurrentBasicBlock );
		elseBlock->moveAfter( thenBlock );
		endBlock->moveAfter( elseBlock );
	}

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformInc16( ptr16 );
	DynamicStore16( final_address, m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformInc8( ptr8 );
	DynamicStore8( final_address, m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformIncDir( const uint32_t address )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), 0x20, "" );
	auto cond = m_IRBuilder.CreateICmpEQ( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x0, true ) ) );
	auto thenBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto elseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto endBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	if ( m_CurrentBasicBlock )
	{
		thenBlock->moveAfter( m_CurrentBasicBlock );
		elseBlock->moveAfter( thenBlock );
		endBlock->moveAfter( elseBlock );
	}

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( direct, 8 ), operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformInc16( ptr16 );
	DynamicStore16(final_address, m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformInc8( ptr8 );
	DynamicStore8( final_address, m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformInc16( llvm::Value* ptr )
{
	auto v = m_IRBuilder.CreateAdd( m_IRBuilder.CreateLoad( ptr, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 1, false ) ), "" );
	m_IRBuilder.CreateStore( v, ptr );
	TestAndSetZero16( v );
	TestAndSetNegative16( v );
}

void Recompiler::PerformInc8( llvm::Value* ptr )
{
	auto v = m_IRBuilder.CreateAdd( m_IRBuilder.CreateLoad( ptr, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 1, false ) ), "" );
	m_IRBuilder.CreateStore( v, ptr );
	TestAndSetZero8( v );
	TestAndSetNegative8( v );
}

void Recompiler::PerformDecAbs( const uint32_t address )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), 0x20, "" );
	auto cond = m_IRBuilder.CreateICmpEQ( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x0, true ) ) );
	auto thenBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto elseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto endBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	if ( m_CurrentBasicBlock )
	{
		thenBlock->moveAfter( m_CurrentBasicBlock );
		elseBlock->moveAfter( thenBlock );
		endBlock->moveAfter( elseBlock );
	}

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformDec16( ptr16 );
	DynamicStore16( final_address, m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformDec8( ptr8 );
	DynamicStore8( final_address, m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformDecDir( const uint32_t address )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), 0x20, "" );
	auto cond = m_IRBuilder.CreateICmpEQ( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x0, true ) ) );
	auto thenBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto elseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto endBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	if ( m_CurrentBasicBlock )
	{
		thenBlock->moveAfter( m_CurrentBasicBlock );
		elseBlock->moveAfter( thenBlock );
		endBlock->moveAfter( elseBlock );
	}

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( direct, 8 ), operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformDec16( ptr16 );
	DynamicStore16( final_address, m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformDec8( ptr8 );
	DynamicStore8( final_address, m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformDec( void )
{
	auto result = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), 0x20, "" );
	auto cond = m_IRBuilder.CreateICmpEQ( result, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x0, true ) ) );
	auto thenBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto elseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto endBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	if ( m_CurrentBasicBlock )
	{
		thenBlock->moveAfter( m_CurrentBasicBlock );
		elseBlock->moveAfter( thenBlock );
		endBlock->moveAfter( elseBlock );
	}
	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	PerformDec16( &m_registerA );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	PerformDec8( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformDec16( llvm::Value* ptr )
{
	auto v = m_IRBuilder.CreateSub( m_IRBuilder.CreateLoad( ptr, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 1, false ) ), "" );
	m_IRBuilder.CreateStore( v, ptr );
	TestAndSetZero16( v );
	TestAndSetNegative16( v );
}

void Recompiler::PerformDec8( llvm::Value* ptr )
{
	auto v = m_IRBuilder.CreateSub( m_IRBuilder.CreateLoad( ptr, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 1, false ) ), "" );
	m_IRBuilder.CreateStore( v, ptr );
	TestAndSetZero8( v );
	TestAndSetNegative8( v );
}

void Recompiler::PerformIny( void )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( X_FLAG );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	PerformInc16( &m_registerY );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	PerformInc8( m_IRBuilder.CreateBitCast( &m_registerY, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformDey( void )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( X_FLAG );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	PerformDec16( &m_registerY );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	PerformDec8( m_IRBuilder.CreateBitCast( &m_registerY, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformAsl( void )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	PerformAsl16( &m_registerA );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	PerformAsl8( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformAslAbs( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformAsl16( m_IRBuilder.CreateLoad( ptr16 ) );
	DynamicStore16( final_address, m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformAsl8( m_IRBuilder.CreateLoad( ptr8 ) );
	DynamicStore8( final_address, m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformAslDir( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( direct, 8 ), operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );

	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformAsl16( ptr16 );
	DynamicStore16( final_address, m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformAsl8( ptr8 );
	DynamicStore8( final_address, m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformAsl16( llvm::Value* ptr )
{
	auto x8000 = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0x8000, false ) );
	auto masked = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerA, "" ), x8000, "" );
	auto carry = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_EQ, masked, x8000, "" );
	auto x1 = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0b00000001, false ) );
	auto complement = m_IRBuilder.CreateXor( x1, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0xff, true ) ) );
	auto unsetP = m_IRBuilder.CreateAnd( complement, m_IRBuilder.CreateLoad( &m_registerP, "" ) );
	auto newP = m_IRBuilder.CreateOr( m_IRBuilder.CreateZExt( carry, llvm::Type::getInt8Ty( m_LLVMContext ) ), unsetP );
	m_IRBuilder.CreateStore( newP, &m_registerP );
	auto v = m_IRBuilder.CreateShl( m_IRBuilder.CreateLoad( ptr, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 1, false ) ), "" );
	m_IRBuilder.CreateStore( v, ptr );
	TestAndSetZero16( v );
	TestAndSetNegative16( v );
}

void Recompiler::PerformAsl8( llvm::Value* ptr )
{
	auto x80 = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x80, false ) );
	auto masked = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ), "" ), x80, "" );
	auto carry = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_EQ, masked, x80, "" );
	auto x1 = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0b00000001, false ) );
	auto complement = m_IRBuilder.CreateXor( x1, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0xff, true ) ) );
	auto unsetP = m_IRBuilder.CreateAnd( complement, m_IRBuilder.CreateLoad( &m_registerP, "" ) );
	auto newP = m_IRBuilder.CreateOr( m_IRBuilder.CreateZExt( carry, llvm::Type::getInt8Ty( m_LLVMContext ) ), unsetP );
	m_IRBuilder.CreateStore( newP, &m_registerP );
	auto v = m_IRBuilder.CreateShl( m_IRBuilder.CreateLoad( ptr, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 1, false ) ), "" );
	m_IRBuilder.CreateStore( v, ptr );
	TestAndSetZero8( v );
	TestAndSetNegative8( v );
}

void Recompiler::PerformLsr( void )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	PerformLsr16( &m_registerA );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	PerformLsr8( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformLsrAbs( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformLsr16( ptr16 );
	DynamicStore16( final_address, m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformLsr8( ptr8 );
	DynamicStore8( final_address, m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformLsrDir( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( direct, 8 ), operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );

	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformLsr16( ptr16 );
	DynamicStore16( final_address, m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto value8 = DynamicLoad8( final_address );
	PerformLsr8( value8 );
	DynamicStore8( final_address, m_IRBuilder.CreateLoad( value8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformLsr16( llvm::Value* ptr )
{
	auto x1 = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0x1, false ) );
	auto masked = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerA, "" ), x1, "" );
	auto carry = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_EQ, masked, x1, "" );
	auto complement = m_IRBuilder.CreateXor( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x1, false ) ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0xff, true ) ) );
	auto unsetP = m_IRBuilder.CreateAnd( complement, m_IRBuilder.CreateLoad( &m_registerP, "" ) );
	auto newP = m_IRBuilder.CreateOr( m_IRBuilder.CreateZExt( carry, llvm::Type::getInt8Ty( m_LLVMContext ) ), unsetP );
	m_IRBuilder.CreateStore( newP, &m_registerP );
	auto v = m_IRBuilder.CreateLShr( m_IRBuilder.CreateLoad( ptr, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 1, false ) ), "" );
	m_IRBuilder.CreateStore( v, ptr );
	TestAndSetZero16( v );
	TestAndSetNegative16( v );
}

void Recompiler::PerformLsr8( llvm::Value* ptr )
{
	auto x1 = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x1, false ) );
	auto masked = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ), "" ), x1, "" );
	auto carry = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_EQ, masked, x1, "" );
	auto complement = m_IRBuilder.CreateXor( x1, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0xff, true ) ) );
	auto unsetP = m_IRBuilder.CreateAnd( complement, m_IRBuilder.CreateLoad( &m_registerP, "" ) );
	auto newP = m_IRBuilder.CreateOr( m_IRBuilder.CreateZExt( carry, llvm::Type::getInt8Ty( m_LLVMContext ) ), unsetP );
	m_IRBuilder.CreateStore( newP, &m_registerP );
	auto v = m_IRBuilder.CreateLShr( m_IRBuilder.CreateLoad( ptr, "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 1, false ) ), "" );
	m_IRBuilder.CreateStore( v, ptr );
	TestAndSetZero8( v );
	TestAndSetNegative8( v );
}

void Recompiler::PerformRol( void )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	PerformRol16( &m_registerA );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	PerformRol8( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformRolAbs( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformRol16( ptr16 );
	DynamicStore16( final_address, m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto value8 = DynamicLoad8( final_address );
	PerformRol8( value8 );
	DynamicStore8( final_address, m_IRBuilder.CreateLoad( value8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformRolDir( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( direct, 8 ), operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );

	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformRol16( ptr16 );
	DynamicStore16( final_address, m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto value8 = DynamicLoad8( final_address );
	PerformRol8( value8 );
	DynamicStore8( final_address, m_IRBuilder.CreateLoad( value8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformRol16( llvm::Value* ptr )
{
	auto x1 = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0b00000001, false ) );
	auto carry = m_IRBuilder.CreateZExt( m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), x1, "" ), llvm::Type::getInt32Ty( m_LLVMContext ) );

	auto data32 = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( ptr ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto result32 = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( data32, 1 ), carry );
	auto newCarry = m_IRBuilder.CreateICmpUGE( result32, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x10000, false ) ) );
	auto complement = m_IRBuilder.CreateXor( x1, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0xff, true ) ) );
	auto unsetP = m_IRBuilder.CreateAnd( complement, m_IRBuilder.CreateLoad( &m_registerP, "" ) );
	auto newP = m_IRBuilder.CreateOr( m_IRBuilder.CreateZExt( newCarry, llvm::Type::getInt8Ty( m_LLVMContext ) ), unsetP );

	m_IRBuilder.CreateStore( newP, &m_registerP );

	auto result16 = m_IRBuilder.CreateTrunc( result32, llvm::Type::getInt16Ty( m_LLVMContext ) );
	m_IRBuilder.CreateStore( result16, ptr );
	TestAndSetZero16( result16 );
	TestAndSetNegative16( result16 );
}

void Recompiler::PerformRol8( llvm::Value* ptr )
{
	auto x1 = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0b00000001, false ) );
	auto carry = m_IRBuilder.CreateZExt( m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), x1, "" ), llvm::Type::getInt16Ty( m_LLVMContext ) );

	auto data16 = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( ptr ), llvm::Type::getInt16Ty( m_LLVMContext ) );
	auto result16 = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( data16, 1 ), carry );
	auto newCarry = m_IRBuilder.CreateICmpUGE( result16, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0x100, false ) ) );
	auto complement = m_IRBuilder.CreateXor( x1, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0xff, true ) ) );
	auto unsetP = m_IRBuilder.CreateAnd( complement, m_IRBuilder.CreateLoad( &m_registerP, "" ) );
	auto newP = m_IRBuilder.CreateOr( m_IRBuilder.CreateZExt( newCarry, llvm::Type::getInt8Ty( m_LLVMContext ) ), unsetP );

	m_IRBuilder.CreateStore( newP, &m_registerP );

	auto result8 = m_IRBuilder.CreateTrunc( result16, llvm::Type::getInt8Ty( m_LLVMContext ) );
	m_IRBuilder.CreateStore( result8, ptr );
	TestAndSetZero8( result8 );
	TestAndSetNegative8( result8 );
}

void Recompiler::PerformRor( void )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	PerformRor16( &m_registerA );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	PerformRor8( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformRorAbs( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformRor16( ptr16 );
	DynamicStore16( final_address, m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformRor8( ptr8 );
	DynamicStore8( final_address, m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformRorDir( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( direct, 8 ), operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );

	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformRor16( ptr16 );
	DynamicStore16( final_address, m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformRor8( ptr8 );
	DynamicStore8( final_address, m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformRor16( llvm::Value* ptr )
{
	auto x1 = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0x1, false ) );
	auto masked = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerA, "" ), x1, "" );
	auto carry = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_EQ, masked, x1, "" );

	auto carryExt = m_IRBuilder.CreateZExt( carry, llvm::Type::getInt32Ty( m_LLVMContext ), "" );
	auto carryExtShifted = m_IRBuilder.CreateShl( carryExt, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 16, false ) ), "" );

	auto aExt = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( ptr, "" ), llvm::Type::getInt32Ty( m_LLVMContext ), "" );
	auto newValueBeforeShift = m_IRBuilder.CreateOr( aExt, carryExtShifted, "" );
	auto newCarry = m_IRBuilder.CreateICmpNE( m_IRBuilder.CreateAnd( newValueBeforeShift, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x1, false ) ), "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0, false ) ), "" );

	auto shifted = m_IRBuilder.CreateLShr( newValueBeforeShift, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 1, false ) ), "" );
	auto complement = m_IRBuilder.CreateXor( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x1, false ) ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0xff, true ) ) );
	auto unsetP = m_IRBuilder.CreateAnd( complement, m_IRBuilder.CreateLoad( &m_registerP, "" ) );
	auto newP = m_IRBuilder.CreateOr( m_IRBuilder.CreateZExt( newCarry, llvm::Type::getInt8Ty( m_LLVMContext ) ), unsetP );
	m_IRBuilder.CreateStore( newP, &m_registerP );
	auto v = m_IRBuilder.CreateTrunc( shifted, llvm::Type::getInt16Ty( m_LLVMContext ) );
	m_IRBuilder.CreateStore( v, ptr, "" );
	TestAndSetZero16( v );
	TestAndSetNegative16( v );
}

void Recompiler::PerformRor8( llvm::Value* ptr )
{
	auto x1 = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x1, false ) );
	auto masked = m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ), "" ), x1, "" );
	auto carry = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_EQ, masked, x1, "" );

	auto carryExt = m_IRBuilder.CreateZExt( carry, llvm::Type::getInt16Ty( m_LLVMContext ), "" );
	auto carryExtShifted = m_IRBuilder.CreateShl( carryExt, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 8, false ) ), "" );

	auto aExt = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( ptr, "" ), llvm::Type::getInt16Ty( m_LLVMContext ), "" );
	auto newValueBeforeShift = m_IRBuilder.CreateOr( aExt, carryExtShifted, "" );

	auto newCarry = m_IRBuilder.CreateICmpNE( m_IRBuilder.CreateAnd( newValueBeforeShift, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0x1, false ) ), "" ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0, false ) ), "" );
	auto shifted = m_IRBuilder.CreateLShr( newValueBeforeShift, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 1, false ) ), "" );
	auto complement = m_IRBuilder.CreateXor( x1, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0xff, true ) ) );
	auto unsetP = m_IRBuilder.CreateAnd( complement, m_IRBuilder.CreateLoad( &m_registerP, "" ) );
	auto newP = m_IRBuilder.CreateOr( m_IRBuilder.CreateZExt( newCarry, llvm::Type::getInt8Ty( m_LLVMContext ) ), unsetP );
	m_IRBuilder.CreateStore( newP, &m_registerP );

	auto v = m_IRBuilder.CreateTrunc( shifted, llvm::Type::getInt8Ty( m_LLVMContext ) );
	m_IRBuilder.CreateStore( v, ptr, "" );
	TestAndSetZero8( v );
	TestAndSetNegative8( v );
}

void Recompiler::PerformTax( void )
{
	PerformRegisterTransfer( &m_registerA, &m_registerX );
}

void Recompiler::PerformTay( void )
{
	PerformRegisterTransfer( &m_registerA, &m_registerY );
}

void Recompiler::PerformTsx( void )
{
	PerformRegisterTransfer( &m_registerSP, &m_registerX );
}

void Recompiler::PerformTxa( void )
{
	PerformRegisterTransfer( &m_registerX, &m_registerA );
}

void Recompiler::PerformTxs( void )
{
	PerformRegisterTransfer( &m_registerX, &m_registerSP );
}

void Recompiler::PerformTxy( void )
{
	PerformRegisterTransfer( &m_registerX, &m_registerY );
}

void Recompiler::PerformTya( void )
{
	PerformRegisterTransfer( &m_registerY, &m_registerA );
}

void Recompiler::PerformTyx( void )
{
	PerformRegisterTransfer( &m_registerY, &m_registerX );
}

void Recompiler::PerformRegisterTransfer( llvm::Value* sourceRegister, llvm::Value* destinationRegister )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( X_FLAG );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto newValue16 = PerformRegisterTransfer16( sourceRegister, destinationRegister );
	TestAndSetZero16( newValue16 );
	TestAndSetNegative16( newValue16 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto newValue8 = PerformRegisterTransfer8( sourceRegister, destinationRegister );
	TestAndSetZero8( newValue8 );
	TestAndSetNegative8( newValue8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

llvm::Value* Recompiler::PerformRegisterTransfer16( llvm::Value* sourceRegister, llvm::Value* destinationRegister )
{
	auto newValue = m_IRBuilder.CreateLoad( sourceRegister, "" );
	m_IRBuilder.CreateStore( newValue, destinationRegister );
	return newValue;
}

llvm::Value* Recompiler::PerformRegisterTransfer8( llvm::Value* sourceRegister, llvm::Value* destinationRegister )
{
	auto newValue = m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( sourceRegister, llvm::Type::getInt8PtrTy( m_LLVMContext ) ), "" );
	m_IRBuilder.CreateStore( newValue, m_IRBuilder.CreateBitCast( destinationRegister, llvm::Type::getInt8PtrTy( m_LLVMContext ) ) );
	return newValue;
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
	auto high16 = m_IRBuilder.CreateLShr( value, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 8, false ) ), "" );
	auto high = m_IRBuilder.CreateTrunc( high16, llvm::Type::getInt8Ty( m_LLVMContext ), "" );
	PushByteToStack( high );

	auto low16 = m_IRBuilder.CreateAnd( value, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0xff, false ) ), "" );
	auto low = m_IRBuilder.CreateTrunc( low16, llvm::Type::getInt8Ty( m_LLVMContext ), "" );
	PushByteToStack( low );
}

void Recompiler::PerformTcs()
{
	auto result = m_IRBuilder.CreateLoad( &m_registerA, "" );
	m_IRBuilder.CreateStore( result, &m_registerSP );
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
	auto a8BeforeSwap = CreateLoadA8();
	auto b8BeforeSwap = CreateLoadB8();

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
	auto mask = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( ~0x1 ), false ) );
	auto masked = m_IRBuilder.CreateAnd( mask, m_IRBuilder.CreateLoad( &m_registerP, "" ), "" );
	m_IRBuilder.CreateStore( masked, &m_registerP );
}

void Recompiler::SetCarry()
{
	auto mask = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( 0x1 ), false ) );
	auto masked = m_IRBuilder.CreateOr( mask, m_IRBuilder.CreateLoad( &m_registerP, "" ), "" );
	m_IRBuilder.CreateStore( masked, &m_registerP );
}

void Recompiler::ClearDecimal()
{
	auto mask = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( ~( 1 << 3) ), false ) );
	auto masked = m_IRBuilder.CreateAnd( mask, m_IRBuilder.CreateLoad( &m_registerP, "" ), "" );
	m_IRBuilder.CreateStore( masked, &m_registerP );
}

void Recompiler::SetDecimal()
{
	auto mask = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( 1 << 3 ), false ) );
	auto masked = m_IRBuilder.CreateOr( mask, m_IRBuilder.CreateLoad( &m_registerP, "" ), "" );
	m_IRBuilder.CreateStore( masked, &m_registerP );
}

void Recompiler::ClearInterrupt()
{
	auto mask = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( ~( 1 << 2 ) ), false ) );
	auto masked = m_IRBuilder.CreateAnd( mask, m_IRBuilder.CreateLoad( &m_registerP, "" ), "" );
	m_IRBuilder.CreateStore( masked, &m_registerP );
}

void Recompiler::SetInterrupt()
{
	auto mask = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( 1 << 2 ), false ) );
	auto masked = m_IRBuilder.CreateOr( mask, m_IRBuilder.CreateLoad( &m_registerP, "" ), "" );
	m_IRBuilder.CreateStore( masked, &m_registerP );
}

void Recompiler::ClearOverflow()
{
	auto mask = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( ~( 1 << 6 ) ), false ) );
	auto masked = m_IRBuilder.CreateAnd( mask, m_IRBuilder.CreateLoad( &m_registerP, "" ), "" );
	m_IRBuilder.CreateStore( masked, &m_registerP );
}

void Recompiler::PerformCmp16( llvm::Value* lValue, llvm::Value* rValue )
{
	auto lValue32 = m_IRBuilder.CreateZExt( rValue, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto rValue32 = m_IRBuilder.CreateZExt( lValue, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto diff = m_IRBuilder.CreateSub( rValue32, lValue32, "" );
	TestAndSetZero16( diff );
	TestAndSetNegative16( diff );
	TestAndSetCarrySubtraction16( diff );
}

void Recompiler::PerformCmp8( llvm::Value* lValue, llvm::Value* rValue )
{
	auto lValue16 = m_IRBuilder.CreateZExt( rValue, llvm::Type::getInt16Ty( m_LLVMContext ) );
	auto rValue16 = m_IRBuilder.CreateZExt( lValue, llvm::Type::getInt16Ty( m_LLVMContext ) );
	auto diff = m_IRBuilder.CreateSub( rValue16, lValue16, "" );
	TestAndSetZero8( diff );
	TestAndSetNegative8( diff );
	TestAndSetCarrySubtraction8( diff );
}

void Recompiler::TestAndSetCarrySubtraction16( llvm::Value* value )
{
	auto isCarry = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_SGE, value, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0, true ) ), "" );
	auto zExtCarry = m_IRBuilder.CreateZExt( isCarry, llvm::Type::getInt8Ty( m_LLVMContext ) );
	auto complement = m_IRBuilder.CreateXor( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x1, true ) ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0xff, true ) ) );
	auto unset = m_IRBuilder.CreateAnd( complement, m_IRBuilder.CreateLoad( &m_registerP, "" ) );
	auto newP = m_IRBuilder.CreateOr( zExtCarry, unset );
	m_IRBuilder.CreateStore( newP, &m_registerP );
}

void Recompiler::TestAndSetCarrySubtraction8( llvm::Value* value )
{
	auto isCarry = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_SGE, value, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0, true ) ), "" );
	auto zExtCarry = m_IRBuilder.CreateZExt( isCarry, llvm::Type::getInt8Ty( m_LLVMContext ) );
	auto complement = m_IRBuilder.CreateXor( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x1, true ) ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0xff, true ) ) );
	auto unset = m_IRBuilder.CreateAnd( complement, m_IRBuilder.CreateLoad( &m_registerP, "" ) );
	auto newP = m_IRBuilder.CreateOr( zExtCarry, unset );
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
	auto writeRegisterY8Bit = m_IRBuilder.CreateBitCast( &m_registerY, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" );
	m_IRBuilder.CreateStore( value, writeRegisterY8Bit );
	TestAndSetZero8( value );
	TestAndSetNegative8( value );
}

void Recompiler::PerformLdx16( llvm::Value* value )
{
	m_IRBuilder.CreateStore( value, &m_registerX );
	TestAndSetZero16( value );
	TestAndSetNegative16( value );
}

void Recompiler::PerformLdx8( llvm::Value* value )
{
	auto writeRegisterX8Bit = m_IRBuilder.CreateBitCast( &m_registerX, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" );
	m_IRBuilder.CreateStore( value, writeRegisterX8Bit );
	TestAndSetZero8( value );
	TestAndSetNegative8( value );
}

void Recompiler::PerformAdcAbsIdxX( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto baseAddress = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	auto x = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerX ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto finalAddress = m_IRBuilder.CreateAdd( baseAddress, x );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( finalAddress );
	PerformAdc16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( finalAddress );
	PerformAdc8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformAdcAbs( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformAdc16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformAdc8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformSbcAbs( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformSbc16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformSbc8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformSbcAbsIdxX( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto baseAddress = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	auto x = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerX ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto finalAddress = m_IRBuilder.CreateAdd( baseAddress, x );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( finalAddress );
	PerformSbc16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( finalAddress );
	PerformSbc8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformSbcDir( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( direct, 8 ), operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );

	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformSbc16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformSbc8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformAdcDir( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( direct, 8 ), operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );

	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformAdc16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformAdc8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformSbcDirIdxX( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto x = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerX ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = m_IRBuilder.CreateAdd( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) ), x );
	auto final_address = m_IRBuilder.CreateAdd( direct, operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );

	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformSbc16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformSbc8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformSbcLongIdxX( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto baseAddress = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto x = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerX ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto finalAddress = m_IRBuilder.CreateAdd( baseAddress, x );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( finalAddress );
	PerformSbc16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( finalAddress );
	PerformSbc8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformAdcLongIdxX( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto baseAddress = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto x = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerX ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto finalAddress = m_IRBuilder.CreateAdd( baseAddress, x );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( finalAddress );
	PerformAdc16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( finalAddress );
	PerformAdc8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformAdcLong( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = StaticLoad16( address );
	PerformAdc16( ptr16 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = StaticLoad8( address );
	PerformAdc8( ptr8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::TestAndSetCarryAddition16( llvm::Value* value )
{
	auto newCarry = m_IRBuilder.CreateICmpUGE( value, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x10000, true ) ) );
	auto mask = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0b00000001, false ) );
	auto complement = m_IRBuilder.CreateXor( mask, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0xff, true ) ) );
	auto unsetP = m_IRBuilder.CreateAnd( complement, m_IRBuilder.CreateLoad( &m_registerP, "" ) );
	auto newP = m_IRBuilder.CreateOr( m_IRBuilder.CreateZExt( newCarry, llvm::Type::getInt8Ty( m_LLVMContext ) ), unsetP );
	m_IRBuilder.CreateStore( newP, &m_registerP );
}

void Recompiler::TestAndSetCarryAddition8( llvm::Value* value )
{
	auto newCarry = m_IRBuilder.CreateICmpUGE( value, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0x0100, true ) ) );
	auto mask = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0b00000001, false ) );
	auto complement = m_IRBuilder.CreateXor( mask, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0xff, true ) ) );
	auto unsetP = m_IRBuilder.CreateAnd( complement, m_IRBuilder.CreateLoad( &m_registerP, "" ) );
	auto newP = m_IRBuilder.CreateOr( m_IRBuilder.CreateZExt( newCarry, llvm::Type::getInt8Ty( m_LLVMContext ) ), unsetP );
	m_IRBuilder.CreateStore( newP, &m_registerP );
}

void Recompiler::TestAndSetOverflowAddition16( llvm::Value* value )
{
	auto outsidePositiveRange = m_IRBuilder.CreateICmpSGT( value, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 32767, true ) ) );
	auto outsideNegativeRange = m_IRBuilder.CreateICmpSLT( value, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, -32768, true ) ) );
	auto newOverflow = m_IRBuilder.CreateShl( m_IRBuilder.CreateZExt( m_IRBuilder.CreateOr( outsideNegativeRange, outsidePositiveRange, "" ), llvm::Type::getInt8Ty( m_LLVMContext ) ), 6 );

	auto mask = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0b01000000, false ) );
	auto complement = m_IRBuilder.CreateXor( mask, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0xff, true ) ) );
	auto unsetP = m_IRBuilder.CreateAnd( complement, m_IRBuilder.CreateLoad( &m_registerP, "" ) );
	auto newP = m_IRBuilder.CreateOr( newOverflow, unsetP );
	m_IRBuilder.CreateStore( newP, &m_registerP );
}

void Recompiler::TestAndSetOverflowAddition8( llvm::Value* value )
{
	auto outsidePositiveRange = m_IRBuilder.CreateICmpSGT( value, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 127, true ) ) );
	auto outsideNegativeRange = m_IRBuilder.CreateICmpSLT( value, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, -128, true ) ) );
	auto newOverflow = m_IRBuilder.CreateShl( m_IRBuilder.CreateZExt( m_IRBuilder.CreateOr( outsideNegativeRange, outsidePositiveRange, "" ), llvm::Type::getInt8Ty( m_LLVMContext ) ), 6 );

	auto mask = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0b01000000, false ) );
	auto complement = m_IRBuilder.CreateXor( mask, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0xff, true ) ) );
	auto unsetP = m_IRBuilder.CreateAnd( complement, m_IRBuilder.CreateLoad( &m_registerP, "" ) );
	auto newP = m_IRBuilder.CreateOr( newOverflow, unsetP );
	m_IRBuilder.CreateStore( newP, &m_registerP );
}

void Recompiler::PerformAdc16( llvm::Value* value )
{
	auto acc32 = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerA, "" ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto value32 = m_IRBuilder.CreateZExt( value, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto result32 = m_IRBuilder.CreateAdd( acc32, value32, "" );
	auto carry = m_IRBuilder.CreateZExt( m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::APInt( 8, static_cast<uint64_t>( 0x1 ), false ) ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	result32 = m_IRBuilder.CreateAdd( result32, carry );

	TestAndSetCarryAddition16( result32 );
	TestAndSetOverflowAddition16( result32 );

	auto newAcc = m_IRBuilder.CreateTrunc( result32, llvm::Type::getInt16Ty( m_LLVMContext ) );
	m_IRBuilder.CreateStore( newAcc, &m_registerA );
	TestAndSetZero16( newAcc );
	TestAndSetNegative16( newAcc );
}

void Recompiler::PerformAdc8( llvm::Value* value )
{
	auto acc16 = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerA, "" ), llvm::Type::getInt16Ty( m_LLVMContext ) );
	auto value16 = m_IRBuilder.CreateZExt( value, llvm::Type::getInt16Ty( m_LLVMContext ) );
	auto result16 = m_IRBuilder.CreateAdd( acc16, value16, "" );
	auto carry = m_IRBuilder.CreateZExt( m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::APInt( 8, static_cast<uint64_t>( 0x1 ), false ) ), llvm::Type::getInt16Ty( m_LLVMContext ) );
	result16 = m_IRBuilder.CreateAdd( result16, carry );

	TestAndSetCarryAddition8( value );
	TestAndSetOverflowAddition8( value );

	auto newAcc = m_IRBuilder.CreateTrunc( result16, llvm::Type::getInt8Ty( m_LLVMContext ) );
	m_IRBuilder.CreateStore( newAcc, m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	TestAndSetZero8( newAcc );
	TestAndSetNegative8( newAcc );
}

void Recompiler::PerformSbc16( llvm::Value* value )
{
	auto acc32 = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerA, "" ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto value32 = m_IRBuilder.CreateZExt( value, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto result32 = m_IRBuilder.CreateSub( acc32, value32, "" );
	auto carry = m_IRBuilder.CreateZExt( m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::APInt( 8, static_cast<uint64_t>( 0x1 ), false ) ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	result32 = m_IRBuilder.CreateAdd( result32, carry );
	result32 = m_IRBuilder.CreateSub( result32, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 1, true ) ) );

	TestAndSetCarrySubtraction16( result32 );
	TestAndSetOverflowAddition8( result32 );

	auto newAcc = m_IRBuilder.CreateTrunc( result32, llvm::Type::getInt16Ty( m_LLVMContext ) );
	m_IRBuilder.CreateStore( newAcc, &m_registerA );
	TestAndSetZero16( newAcc );
	TestAndSetNegative16( newAcc );
}

void Recompiler::PerformSbc8( llvm::Value* value )
{
	auto acc16 = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerA, "" ), llvm::Type::getInt16Ty( m_LLVMContext ) );
	auto value16 = m_IRBuilder.CreateZExt( value, llvm::Type::getInt16Ty( m_LLVMContext ) );
	auto result16 = m_IRBuilder.CreateSub( acc16, value16, "" );
	auto carry = m_IRBuilder.CreateZExt( m_IRBuilder.CreateAnd( m_IRBuilder.CreateLoad( &m_registerP, "" ), llvm::APInt( 8, static_cast<uint64_t>( 0x1 ), false ) ), llvm::Type::getInt16Ty( m_LLVMContext ) );
	result16 = m_IRBuilder.CreateAdd( result16, carry );
	result16 = m_IRBuilder.CreateSub( result16, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 1, true ) ) );

	TestAndSetCarrySubtraction8( result16 );
	TestAndSetOverflowAddition8( result16 );

	auto newAcc = m_IRBuilder.CreateTrunc( result16, llvm::Type::getInt8Ty( m_LLVMContext ) );
	m_IRBuilder.CreateStore( newAcc, m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	TestAndSetZero8( newAcc );
	TestAndSetNegative8( newAcc );
}

void Recompiler::PerformLda16( llvm::Value* value )
{
	m_IRBuilder.CreateStore( value, &m_registerA );
	TestAndSetZero16( value );
	TestAndSetNegative16( value );
}

void Recompiler::PerformLda8( llvm::Value* value )
{
	auto writeRegisterA8Bit = m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" );
	m_IRBuilder.CreateStore( value, writeRegisterA8Bit );
	TestAndSetZero8( value );
	TestAndSetNegative8( value );
}

void Recompiler::PerformOra16( llvm::Value* value )
{
	auto loadA = m_IRBuilder.CreateLoad( &m_registerA, "" );
	auto newA = m_IRBuilder.CreateOr( loadA, value, "" );
	m_IRBuilder.CreateStore( newA, &m_registerA );
	TestAndSetZero16( newA );
	TestAndSetNegative16( newA );
}

void Recompiler::PerformOra8( llvm::Value* value )
{
	auto loadA = m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	auto newA = m_IRBuilder.CreateOr( loadA, value, "" );
	m_IRBuilder.CreateStore( newA, m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	TestAndSetZero8( newA );
	TestAndSetNegative8( newA );
}

void Recompiler::PerformBit16Imm( llvm::Value* value )
{
	auto loadA = m_IRBuilder.CreateLoad( &m_registerA, "" );
	auto result = m_IRBuilder.CreateAnd( loadA, value, "" );
	TestAndSetZero16( result );
}

void Recompiler::PerformBit8Imm( llvm::Value* value )
{
	auto loadA = m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	auto result = m_IRBuilder.CreateAnd( loadA, value, "" );
	TestAndSetZero8( result );
}

void Recompiler::PerformAnd16( llvm::Value* value )
{
	auto loadA = m_IRBuilder.CreateLoad( &m_registerA, "" );
	auto newA = m_IRBuilder.CreateAnd( loadA, value, "" );
	m_IRBuilder.CreateStore( newA, &m_registerA );
	TestAndSetZero16( newA );
	TestAndSetNegative16( newA );
}

void Recompiler::PerformAnd8( llvm::Value* value )
{
	auto loadA = m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	auto newA = m_IRBuilder.CreateAnd( loadA, value, "" );
	m_IRBuilder.CreateStore( newA, m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	TestAndSetZero8( newA );
	TestAndSetNegative8( newA );
}

void Recompiler::PerformEor16( llvm::Value* value )
{
	auto loadA = m_IRBuilder.CreateLoad( &m_registerA, "" );
	auto newA = m_IRBuilder.CreateXor( loadA, value, "" );
	m_IRBuilder.CreateStore( newA, &m_registerA );
	TestAndSetZero16( newA );
	TestAndSetNegative16( newA );
}

void Recompiler::PerformEor8( llvm::Value* value )
{
	auto loadA = m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	auto newA = m_IRBuilder.CreateXor( loadA, value, "" );
	m_IRBuilder.CreateStore( newA, m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	TestAndSetZero8( newA );
	TestAndSetNegative8( newA );
}

void Recompiler::TestAndSetOverflow16( llvm::Value* value )
{
	auto x4000 = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0x4000, false ) );
	auto masked = m_IRBuilder.CreateAnd( value, x4000, "" );
	auto isOverflow = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_EQ, masked, x4000, "" );
	auto zExtisOverflow = m_IRBuilder.CreateZExt( isOverflow, llvm::Type::getInt8Ty( m_LLVMContext ) );
	auto zExtisOverflowShifted = m_IRBuilder.CreateShl( zExtisOverflow, 7 );
	auto complement = m_IRBuilder.CreateXor( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x40, false ) ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0xff, true ) ) );
	auto unset = m_IRBuilder.CreateAnd( complement, m_IRBuilder.CreateLoad( &m_registerP, "" ) );
	auto newP = m_IRBuilder.CreateOr( zExtisOverflowShifted, unset );

	m_IRBuilder.CreateStore( newP, &m_registerP );
}

void Recompiler::TestAndSetOverflow8( llvm::Value* value )
{
	auto x40 = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x40, false ) );
	auto masked = m_IRBuilder.CreateAnd( value, x40, "" );

	auto complement = m_IRBuilder.CreateXor( x40, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0xff, true ) ) );
	auto unset = m_IRBuilder.CreateAnd( complement, m_IRBuilder.CreateLoad( &m_registerP, "" ) );
	auto newP = m_IRBuilder.CreateOr( masked, unset );

	m_IRBuilder.CreateStore( newP, &m_registerP );
}

void Recompiler::TestAndSetZero16( llvm::Value* value )
{
	auto zeroConst = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0, false ) );
	auto isZero = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_EQ, value, zeroConst, "" );
	auto zExtIsZero = m_IRBuilder.CreateZExt( isZero, llvm::Type::getInt8Ty( m_LLVMContext ) );
	auto shiftedIsZero = m_IRBuilder.CreateShl( zExtIsZero, 1 );
	auto complement = m_IRBuilder.CreateXor( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x2, true ) ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0xff, true ) ) );
	auto unset = m_IRBuilder.CreateAnd( complement, m_IRBuilder.CreateLoad( &m_registerP, "" ) );
	auto newP = m_IRBuilder.CreateOr( shiftedIsZero, unset );

	m_IRBuilder.CreateStore( newP, &m_registerP );
}

void Recompiler::TestAndSetZero8( llvm::Value* value )
{
	auto zeroConst = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0, false ) );
	auto isZero = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_EQ, value, zeroConst, "" );
	auto zExtIsZero = m_IRBuilder.CreateZExt( isZero, llvm::Type::getInt8Ty( m_LLVMContext ) );
	auto shiftedIsZero = m_IRBuilder.CreateShl( zExtIsZero, 1 );
	auto complement = m_IRBuilder.CreateXor( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x2, true ) ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0xff, true ) ) );
	auto unset = m_IRBuilder.CreateAnd( complement, m_IRBuilder.CreateLoad( &m_registerP, "" ) );
	auto newP = m_IRBuilder.CreateOr( shiftedIsZero, unset );

	m_IRBuilder.CreateStore( newP, &m_registerP );
}

void Recompiler::TestAndSetNegative16( llvm::Value* value )
{
	auto x8000 = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0x8000, false ) );
	auto masked = m_IRBuilder.CreateAnd( value, x8000, "" );
	auto isNeg = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_EQ, masked, x8000, "" );
	auto zExtisNeg = m_IRBuilder.CreateZExt( isNeg, llvm::Type::getInt8Ty( m_LLVMContext ) );
	auto zExtisNegShifted = m_IRBuilder.CreateShl( zExtisNeg, 7 );
	auto complement = m_IRBuilder.CreateXor( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x80, false ) ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0xff, true ) ) );
	auto unset = m_IRBuilder.CreateAnd( complement, m_IRBuilder.CreateLoad( &m_registerP, "" ) );
	auto newP = m_IRBuilder.CreateOr( zExtisNegShifted, unset );

	m_IRBuilder.CreateStore( newP, &m_registerP );
}

void Recompiler::TestAndSetNegative8( llvm::Value* value )
{
	auto x80 = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x80, false ) );
	auto masked = m_IRBuilder.CreateAnd( value, x80, "" );

	auto complement = m_IRBuilder.CreateXor( x80, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0xff, true ) ) );
	auto unset = m_IRBuilder.CreateAnd( complement, m_IRBuilder.CreateLoad( &m_registerP, "" ) );
	auto newP = m_IRBuilder.CreateOr( masked, unset );

	m_IRBuilder.CreateStore( newP, &m_registerP );
}

llvm::Value* Recompiler::ComputeNewPC( llvm::Value* payloadSize )
{
	auto pc = m_IRBuilder.CreateLoad( &m_registerPC, "" );
	auto newPc = m_IRBuilder.CreateAdd( pc, payloadSize );
	return newPc;
}

llvm::Value* Recompiler::wRamPtr16( const uint32_t offset )
{
	assert( offset < WRAM_SIZE );
	std::vector<llvm::Value*> idxList = { llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0, false ) ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( offset ), false ) ) };
	auto ptr = m_IRBuilder.CreateInBoundsGEP( m_wRam.getType()->getPointerElementType(), &m_wRam, idxList );
	return m_IRBuilder.CreateBitCast( ptr, llvm::Type::getInt16PtrTy( m_LLVMContext ), "" );
}

llvm::Value* Recompiler::wRamPtr8( const uint32_t offset )
{
	assert( offset < WRAM_SIZE );
	return  m_IRBuilder.CreateConstGEP2_32( m_wRam.getType()->getPointerElementType(), &m_wRam, 0, offset, "" );
}

llvm::Value* Recompiler::romPtr16( const uint32_t offset )
{
	assert( offset < ROM_SIZE );
	auto ptr =  m_IRBuilder.CreateConstGEP2_32( llvm::Type::getInt16PtrTy( m_LLVMContext ), &m_Rom, 0, offset, "" );
	return m_IRBuilder.CreateBitCast( ptr, llvm::Type::getInt16PtrTy( m_LLVMContext ), "" );
}

llvm::Value* Recompiler::romPtr8( const uint32_t offset )
{
	assert( offset < ROM_SIZE );
	return  m_IRBuilder.CreateConstGEP2_32( m_Rom.getType()->getPointerElementType(), &m_Rom, 0, offset, "" );
}

llvm::Value* Recompiler::wRamPtr16( llvm::Value* offset )
{
	assert( offset < WRAM_SIZE );
	std::vector<llvm::Value*> idxList = { llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0, false ) ), offset };
	auto ptr = m_IRBuilder.CreateInBoundsGEP( m_wRam.getType()->getPointerElementType(), &m_wRam, idxList );
	return m_IRBuilder.CreateBitCast( ptr, llvm::Type::getInt16PtrTy( m_LLVMContext ), "" );
}

llvm::Value* Recompiler::wRamPtr8( llvm::Value* offset )
{
	assert( offset < WRAM_SIZE );
	std::vector<llvm::Value*> idxList = { llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0, false ) ), offset };
	return m_IRBuilder.CreateInBoundsGEP( m_wRam.getType()->getPointerElementType(), &m_wRam, idxList );
}

llvm::Value* Recompiler::romPtr16( llvm::Value* offset )
{
	assert( offset < ROM_SIZE );
	std::vector<llvm::Value*> idxList = { llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0, false ) ), offset };
	auto ptr = m_IRBuilder.CreateInBoundsGEP( m_Rom.getType()->getPointerElementType(), &m_Rom, idxList );
	return m_IRBuilder.CreateBitCast( ptr, llvm::Type::getInt16PtrTy( m_LLVMContext ), "" );
}

llvm::Value* Recompiler::romPtr8( llvm::Value* offset )
{
	assert( offset < ROM_SIZE );
	std::vector<llvm::Value*> idxList = { llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0, false ) ), offset };
	return m_IRBuilder.CreateInBoundsGEP( m_Rom.getType()->getPointerElementType(), &m_Rom, idxList );
}

void Recompiler::PerformLdaDir( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( direct, 8 ), operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );

	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformLda16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformLda8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformStzDirIdxX( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto x = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerX ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = m_IRBuilder.CreateAdd( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) ), x );
	auto final_address = m_IRBuilder.CreateAdd( direct, operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	DynamicStore16( final_address, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0, false ) ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	DynamicStore8( final_address, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0, false ) ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformStyDirIdxX( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( X_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto x = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerX ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = m_IRBuilder.CreateAdd( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) ), x );
	auto final_address = m_IRBuilder.CreateAdd( direct, operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	DynamicStore16( final_address, m_IRBuilder.CreateLoad( &m_registerY ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	DynamicStore8( final_address, m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerY, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformStxDirIdxY( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( X_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto y = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerY ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = m_IRBuilder.CreateAdd( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) ), y );
	auto final_address = m_IRBuilder.CreateAdd( direct, operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	DynamicStore16( final_address, m_IRBuilder.CreateLoad( &m_registerX ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	DynamicStore8( final_address, m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerX, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformStaDirIdxX( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto x = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerX ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = m_IRBuilder.CreateAdd( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) ), x );
	auto final_address = m_IRBuilder.CreateAdd( direct, operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	DynamicStore16( final_address, m_IRBuilder.CreateLoad( &m_registerA ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	DynamicStore8( final_address, m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformLdxDirIdxY( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( X_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto y = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerY ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = m_IRBuilder.CreateAdd( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) ), y );
	auto final_address = m_IRBuilder.CreateAdd( direct, operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformLdx16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformLdx8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformLdyDirIdxX( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( X_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto x = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerX ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = m_IRBuilder.CreateAdd( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) ), x );
	auto final_address = m_IRBuilder.CreateAdd( direct, operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformLdy16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformLdy8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformLdaDirIdxX( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto x = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerX ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = m_IRBuilder.CreateAdd( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) ), x);
	auto final_address = m_IRBuilder.CreateAdd( direct, operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformLda16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformLda8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformStzAbsIdxX( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto baseAddress = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	auto x = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerX ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto finalAddress = m_IRBuilder.CreateAdd( baseAddress, x );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	DynamicStore16( finalAddress, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0, false ) ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	DynamicStore8( finalAddress, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0, false ) ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformStaAbsIdxX( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto baseAddress = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	auto x = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerX ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto finalAddress = m_IRBuilder.CreateAdd( baseAddress, x );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	DynamicStore16( finalAddress, m_IRBuilder.CreateLoad( &m_registerA ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	DynamicStore8( finalAddress, m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformStaAbsIdxY( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto baseAddress = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	auto y = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerY ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto finalAddress = m_IRBuilder.CreateAdd( baseAddress, y );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	DynamicStore16( finalAddress, m_IRBuilder.CreateLoad( &m_registerA ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	DynamicStore8( finalAddress, m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformLdyAbsIdxX( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( X_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto baseAddress = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	auto x = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerX ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto finalAddress = m_IRBuilder.CreateAdd( baseAddress, x );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( finalAddress );
	PerformLdy16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( finalAddress );
	PerformLdy8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformLdxAbsIdxY( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( X_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto baseAddress = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	auto y = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerY ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto finalAddress = m_IRBuilder.CreateAdd( baseAddress, y );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( finalAddress );
	PerformLdx16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( finalAddress );
	PerformLdx8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformLdaAbsIdxX( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto baseAddress = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	auto x = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerX ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto finalAddress = m_IRBuilder.CreateAdd( baseAddress, x );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( finalAddress );
	PerformLda16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( finalAddress );
	PerformLda8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformLdaAbsIdxY( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto baseAddress = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	auto y = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerY ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto finalAddress = m_IRBuilder.CreateAdd( baseAddress, y );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( finalAddress );
	PerformLda16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( finalAddress );
	PerformLda8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformLdaAbs( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformLda16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformLda8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformLdxAbs( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( X_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformLdx16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformLdx8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformLdxDir( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( X_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( direct, 8 ), operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformLdx16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformLdx8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformLdyAbs( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( X_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformLdy16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformLdy8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformLdyDir( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( X_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( direct, 8 ), operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformLdy16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformLdy8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformTsbAbs( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformTsb16( ptr16 );
	DynamicStore16( final_address, m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformTsb8( ptr8 );
	DynamicStore8( final_address, m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformTsbDir( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( direct, 8 ), operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformTsb16( ptr16 );
	DynamicStore16( final_address, m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformTsb8( ptr8 );
	DynamicStore8( final_address, m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformTrbDir( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( direct, 8 ), operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformTrb16( ptr16 );
	DynamicStore16( final_address, m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformTrb8( ptr8 );
	DynamicStore8( final_address, m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformTrbAbs( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformTrb16( ptr16 );
	DynamicStore16( final_address, m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformTrb8( ptr8 );
	DynamicStore8( final_address, m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformTsb16( llvm::Value* ptr )
{
	auto value = m_IRBuilder.CreateLoad( ptr );
	auto acc = m_IRBuilder.CreateLoad( &m_registerA );
	TestAndSetZero16( m_IRBuilder.CreateAnd( value, acc ) );
	auto newValue = m_IRBuilder.CreateOr( value, acc );
	m_IRBuilder.CreateStore( newValue, ptr );
}

void Recompiler::PerformTsb8( llvm::Value* ptr )
{
	auto value = m_IRBuilder.CreateLoad( ptr );
	auto acc = m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ) ) );
	TestAndSetZero8( m_IRBuilder.CreateAnd( value, acc ) );
	auto newValue = m_IRBuilder.CreateOr( value, acc );
	m_IRBuilder.CreateStore( newValue, ptr );
}

void Recompiler::PerformTrb16( llvm::Value* ptr )
{
	auto value = m_IRBuilder.CreateLoad( ptr );
	auto acc = m_IRBuilder.CreateLoad( &m_registerA );
	TestAndSetZero16( m_IRBuilder.CreateAnd( value, acc ) );
	auto newValue = m_IRBuilder.CreateAnd( value, m_IRBuilder.CreateXor( acc, 0xffff ) );
	m_IRBuilder.CreateStore( newValue, ptr );
}

void Recompiler::PerformTrb8( llvm::Value* ptr )
{
	auto value = m_IRBuilder.CreateLoad( ptr );
	auto acc = m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ) ) );
	TestAndSetZero8( m_IRBuilder.CreateAnd( value, acc ) );
	auto newValue = m_IRBuilder.CreateAnd( value, m_IRBuilder.CreateXor( acc, 0xff ) );
	m_IRBuilder.CreateStore( newValue, ptr );
}

void Recompiler::PerformAndDir( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( direct, 8 ), operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformAnd16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformAnd8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformAndAbs( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformAnd16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformAnd8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformEorAbs( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformEor16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformEor8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformEorDirIdxX( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto x = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerX ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = m_IRBuilder.CreateAdd( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) ), x );
	auto final_address = m_IRBuilder.CreateAdd( direct, operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformEor16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformEor8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformEorDir( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( direct, 8 ), operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformEor16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformEor8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformCmpAbs( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformCmp16( m_IRBuilder.CreateLoad( ptr16 ), CreateLoadA16() );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformCmp8( m_IRBuilder.CreateLoad( ptr8 ), CreateLoadA8() );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformCmpDir( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( direct, 8 ), operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformCmp16( m_IRBuilder.CreateLoad( ptr16 ), CreateLoadA16() );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformCmp8( m_IRBuilder.CreateLoad( ptr8 ), CreateLoadA8() );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformCpxAbs( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( X_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformCmp16( m_IRBuilder.CreateLoad( ptr16 ), CreateLoadX16() );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformCmp8( m_IRBuilder.CreateLoad( ptr8 ), CreateLoadX8() );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformCpxDir( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( X_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( direct, 8 ), operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformCmp16( m_IRBuilder.CreateLoad( ptr16 ), CreateLoadX16() );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformCmp8( m_IRBuilder.CreateLoad( ptr8 ), CreateLoadX8() );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformCpyAbs( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( X_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformCmp16( m_IRBuilder.CreateLoad( ptr16 ), CreateLoadY16() );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformCmp8( m_IRBuilder.CreateLoad( ptr8 ), CreateLoadY8() );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformCpyDir( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( X_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( direct, 8 ), operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformCmp16( m_IRBuilder.CreateLoad( ptr16 ), CreateLoadY16() );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformCmp8( m_IRBuilder.CreateLoad( ptr8 ), CreateLoadY8() );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformOraAbsIdxY( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto baseAddress = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	auto y = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerY ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto finalAddress = m_IRBuilder.CreateAdd( baseAddress, y );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( finalAddress );
	PerformOra16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( finalAddress );
	PerformOra8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformOraAbs( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformOra16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformOra8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformOraDir( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( direct, 8 ), operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformOra16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformOra8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformBitDir( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( direct, 8 ), operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformBit16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformBit8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformBitAbs( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( final_address );
	PerformBit16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( final_address );
	PerformBit8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformBit16( llvm::Value* value )
{
	TestAndSetOverflow16( value );
	TestAndSetNegative16( value );
	auto acc = m_IRBuilder.CreateLoad( &m_registerA );
	TestAndSetZero16( m_IRBuilder.CreateAnd( acc, value ) );
}

void Recompiler::PerformBit8( llvm::Value* value )
{
	TestAndSetOverflow8( value );
	TestAndSetNegative8( value );
	auto acc = m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ) ) );
	TestAndSetZero8( m_IRBuilder.CreateAnd( acc, value ) );
}

void Recompiler::PerformStaDir( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( direct, 8 ), operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	DynamicStore16( final_address, m_IRBuilder.CreateLoad( &m_registerA ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	DynamicStore8( final_address, m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformStxDir( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( X_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( direct, 8 ), operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	DynamicStore16( final_address, m_IRBuilder.CreateLoad( &m_registerX ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	DynamicStore8( final_address, m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerX, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformStyDir( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( X_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( direct, 8 ), operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	DynamicStore16( final_address, m_IRBuilder.CreateLoad( &m_registerY ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	DynamicStore8( final_address, m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerY, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformMvn( const uint32_t operand, const uint32_t instructionOffset, const std::string& instructionString )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	PerformMvn16( operand, instructionOffset, instructionString );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	PerformMvn8( operand, instructionOffset, instructionString );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformMvn16( const uint32_t operand, const uint32_t instructionOffset, const std::string& instructionString )
{
	auto destinationBank = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( operand & 0xff ), false ) );
	auto shiftedBank = m_IRBuilder.CreateShl( destinationBank, 16 );
	m_IRBuilder.CreateStore( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( operand & 0xff ), false ) ), &m_registerDB );

	auto sourceBank = m_IRBuilder.CreateShl( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( operand & 0xff00 ), false ) ), 16 );

	auto mvnBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto updateInstructionOutputBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto exitBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	mvnBlock->moveAfter( m_CurrentBasicBlock );
	updateInstructionOutputBlock->moveAfter( mvnBlock );
	exitBlock->moveAfter( updateInstructionOutputBlock );
	
	m_IRBuilder.CreateBr( mvnBlock );
	
	SelectBlock( mvnBlock );
	auto x = m_IRBuilder.CreateLoad( &m_registerX );
	auto y = m_IRBuilder.CreateLoad( &m_registerY );
	auto readPtr = DynamicLoad16( m_IRBuilder.CreateOr( sourceBank, m_IRBuilder.CreateZExt( x, llvm::Type::getInt32Ty( m_LLVMContext ) ) ) );
	auto writePtr = DynamicLoad16( m_IRBuilder.CreateOr( shiftedBank, m_IRBuilder.CreateZExt( y, llvm::Type::getInt32Ty( m_LLVMContext ) ) ) );

	m_IRBuilder.CreateStore( m_IRBuilder.CreateLoad( readPtr ), writePtr );

	m_IRBuilder.CreateStore( m_IRBuilder.CreateAdd( x, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( 1 ), true ) ) ), &m_registerX );
	m_IRBuilder.CreateStore( m_IRBuilder.CreateAdd( y, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( 1 ), true ) ) ), &m_registerY );
	m_IRBuilder.CreateStore( m_IRBuilder.CreateSub( m_IRBuilder.CreateLoad( &m_registerA ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( 1 ), true ) ) ), &m_registerA );

	auto cond = m_IRBuilder.CreateICmpNE( m_IRBuilder.CreateLoad( &m_registerA ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( 0xffff ), true ) ) );
	m_IRBuilder.CreateCondBr( cond, updateInstructionOutputBlock, exitBlock );

	SelectBlock( updateInstructionOutputBlock );
	PerformUpdateInstructionOutput( instructionOffset, instructionString );
	PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), m_IRBuilder.CreateLoad( &m_registerPC, "" ) );
	m_IRBuilder.CreateBr( mvnBlock );

	SelectBlock( exitBlock );
}

void Recompiler::PerformMvn8( const uint32_t operand, const uint32_t instructionOffset, const std::string& instructionString )
{
	auto destinationBank = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( operand & 0xff ), false ) );
	auto shiftedBank = m_IRBuilder.CreateShl( destinationBank, 16 );
	m_IRBuilder.CreateStore( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( operand & 0xff ), false ) ), &m_registerDB );

	auto sourceBank = m_IRBuilder.CreateShl( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( operand & 0xff00 ), false ) ), 16 );

	auto mvnBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto updateInstructionOutputBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto exitBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	mvnBlock->moveAfter( m_CurrentBasicBlock );
	updateInstructionOutputBlock->moveAfter( mvnBlock );
	exitBlock->moveAfter( updateInstructionOutputBlock );

	m_IRBuilder.CreateBr( mvnBlock );

	SelectBlock( mvnBlock );
	auto x = m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerX, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	auto y = m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerY, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	auto readPtr = DynamicLoad16( m_IRBuilder.CreateOr( sourceBank, m_IRBuilder.CreateZExt( x, llvm::Type::getInt32Ty( m_LLVMContext ) ) ) );
	auto writePtr = DynamicLoad16( m_IRBuilder.CreateOr( shiftedBank, m_IRBuilder.CreateZExt( y, llvm::Type::getInt32Ty( m_LLVMContext ) ) ) );

	m_IRBuilder.CreateStore( m_IRBuilder.CreateLoad( readPtr ), writePtr );

	m_IRBuilder.CreateStore( m_IRBuilder.CreateAdd( x, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( 1 ), true ) ) ), m_IRBuilder.CreateBitCast( &m_registerX, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	m_IRBuilder.CreateStore( m_IRBuilder.CreateAdd( y, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( 1 ), true ) ) ), m_IRBuilder.CreateBitCast( &m_registerY, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	m_IRBuilder.CreateStore( m_IRBuilder.CreateSub( m_IRBuilder.CreateLoad( &m_registerA ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( 1 ), true ) ) ), &m_registerA );

	auto cond = m_IRBuilder.CreateICmpNE( m_IRBuilder.CreateLoad( &m_registerA ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( 0xffff ), true ) ) );
	m_IRBuilder.CreateCondBr( cond, updateInstructionOutputBlock, exitBlock );

	SelectBlock( updateInstructionOutputBlock );
	PerformUpdateInstructionOutput( instructionOffset, instructionString );
	PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), m_IRBuilder.CreateLoad( &m_registerPC, "" ) );
	m_IRBuilder.CreateBr( mvnBlock );

	SelectBlock( exitBlock );
}

void Recompiler::PerformMvp( const uint32_t operand, const uint32_t instructionOffset, const std::string& instructionString )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	PerformMvp16( operand, instructionOffset, instructionString );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	PerformMvp8( operand, instructionOffset, instructionString );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformMvp16( const uint32_t operand, const uint32_t instructionOffset, const std::string& instructionString )
{
	auto destinationBank = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( operand & 0xff ), false ) );
	auto shiftedBank = m_IRBuilder.CreateShl( destinationBank, 16 );
	m_IRBuilder.CreateStore( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( operand & 0xff ), false ) ), &m_registerDB );

	auto sourceBank = m_IRBuilder.CreateShl( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( operand & 0xff00 ), false ) ), 16 );

	auto mvnBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto updateInstructionOutputBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto exitBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	mvnBlock->moveAfter( m_CurrentBasicBlock );
	updateInstructionOutputBlock->moveAfter( mvnBlock );
	exitBlock->moveAfter( updateInstructionOutputBlock );

	m_IRBuilder.CreateBr( mvnBlock );

	SelectBlock( mvnBlock );
	auto x = m_IRBuilder.CreateLoad( &m_registerX );
	auto y = m_IRBuilder.CreateLoad( &m_registerY );
	auto readPtr = DynamicLoad16( m_IRBuilder.CreateOr( sourceBank, m_IRBuilder.CreateZExt( x, llvm::Type::getInt32Ty( m_LLVMContext ) ) ) );
	auto writePtr = DynamicLoad16( m_IRBuilder.CreateOr( shiftedBank, m_IRBuilder.CreateZExt( y, llvm::Type::getInt32Ty( m_LLVMContext ) ) ) );

	m_IRBuilder.CreateStore( m_IRBuilder.CreateLoad( readPtr ), writePtr );

	m_IRBuilder.CreateStore( m_IRBuilder.CreateSub( x, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( 1 ), true ) ) ), &m_registerX );
	m_IRBuilder.CreateStore( m_IRBuilder.CreateSub( y, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( 1 ), true ) ) ), &m_registerY );
	m_IRBuilder.CreateStore( m_IRBuilder.CreateSub( m_IRBuilder.CreateLoad( &m_registerA ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( 1 ), true ) ) ), &m_registerA );

	auto cond = m_IRBuilder.CreateICmpNE( m_IRBuilder.CreateLoad( &m_registerA ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( 0xffff ), true ) ) );
	m_IRBuilder.CreateCondBr( cond, updateInstructionOutputBlock, exitBlock );

	SelectBlock( updateInstructionOutputBlock );
	PerformUpdateInstructionOutput( instructionOffset, instructionString );
	PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), m_IRBuilder.CreateLoad( &m_registerPC, "" ) );
	m_IRBuilder.CreateBr( mvnBlock );

	SelectBlock( exitBlock );
}

void Recompiler::PerformMvp8( const uint32_t operand, const uint32_t instructionOffset, const std::string& instructionString )
{
	auto destinationBank = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( operand & 0xff ), false ) );
	auto shiftedBank = m_IRBuilder.CreateShl( destinationBank, 16 );
	m_IRBuilder.CreateStore( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( operand & 0xff ), false ) ), &m_registerDB );

	auto sourceBank = m_IRBuilder.CreateShl( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( operand & 0xff00 ), false ) ), 16 );

	auto mvnBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto updateInstructionOutputBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto exitBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	mvnBlock->moveAfter( m_CurrentBasicBlock );
	updateInstructionOutputBlock->moveAfter( mvnBlock );
	exitBlock->moveAfter( updateInstructionOutputBlock );

	m_IRBuilder.CreateBr( mvnBlock );

	SelectBlock( mvnBlock );
	auto x = m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerX, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	auto y = m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerY, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	auto readPtr = DynamicLoad16( m_IRBuilder.CreateOr( sourceBank, m_IRBuilder.CreateZExt( x, llvm::Type::getInt32Ty( m_LLVMContext ) ) ) );
	auto writePtr = DynamicLoad16( m_IRBuilder.CreateOr( shiftedBank, m_IRBuilder.CreateZExt( y, llvm::Type::getInt32Ty( m_LLVMContext ) ) ) );

	m_IRBuilder.CreateStore( m_IRBuilder.CreateLoad( readPtr ), writePtr );

	m_IRBuilder.CreateStore( m_IRBuilder.CreateSub( x, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( 1 ), true ) ) ), m_IRBuilder.CreateBitCast( &m_registerX, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	m_IRBuilder.CreateStore( m_IRBuilder.CreateSub( y, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( 1 ), true ) ) ), m_IRBuilder.CreateBitCast( &m_registerY, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) );
	m_IRBuilder.CreateStore( m_IRBuilder.CreateSub( m_IRBuilder.CreateLoad( &m_registerA ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( 1 ), true ) ) ), &m_registerA );

	auto cond = m_IRBuilder.CreateICmpNE( m_IRBuilder.CreateLoad( &m_registerA ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, static_cast<uint64_t>( 0xffff ), true ) ) );
	m_IRBuilder.CreateCondBr( cond, updateInstructionOutputBlock, exitBlock );

	SelectBlock( updateInstructionOutputBlock );
	PerformUpdateInstructionOutput( instructionOffset, instructionString );
	PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ), m_IRBuilder.CreateLoad( &m_registerPC, "" ) );
	m_IRBuilder.CreateBr( mvnBlock );

	SelectBlock( exitBlock );
}

void Recompiler::PerformStzDir( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( direct, 8 ), operand_address );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	DynamicStore16( final_address, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0, false ) ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	DynamicStore8( final_address, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0, false ) ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformStaAbs( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	DynamicStore16( final_address, m_IRBuilder.CreateLoad( &m_registerA ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	DynamicStore8( final_address, m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformStxAbs( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( X_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	DynamicStore16( final_address, m_IRBuilder.CreateLoad( &m_registerX ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	DynamicStore8( final_address, m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerX, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformStyAbs( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( X_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	DynamicStore16( final_address, m_IRBuilder.CreateLoad( &m_registerY ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	DynamicStore8( final_address, m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerY, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformStzAbs( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto bank = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDB ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto bank_offset = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto final_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( bank, 16 ), bank_offset );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	DynamicStore16( final_address, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0, false ) ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	DynamicStore8( final_address, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0, false ) ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformStaLongIdxX( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto baseAddress = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto x = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerX ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto finalAddress = m_IRBuilder.CreateAdd( baseAddress, x );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	DynamicStore16( finalAddress, m_IRBuilder.CreateLoad( &m_registerA ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	DynamicStore8( finalAddress, m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformStaLong( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto final_address = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	DynamicStore16( final_address, m_IRBuilder.CreateLoad( &m_registerA ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	DynamicStore8( final_address, m_IRBuilder.CreateLoad( m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ), "" ) ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformLdaDirIndLngIdxY( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	auto direct = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerDP ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto operand_address = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto base_address = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( direct, 8 ), operand_address );

	auto low = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( DynamicLoad8( base_address ) ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto mid = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( DynamicLoad8( m_IRBuilder.CreateAdd( base_address, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 1, false ) ) ) ) ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto high = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( DynamicLoad8( m_IRBuilder.CreateAdd( base_address, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 2, false ) ) ) ) ), llvm::Type::getInt32Ty( m_LLVMContext ) );

	auto highShifted = m_IRBuilder.CreateShl( high, 16 );
	auto midShifted = m_IRBuilder.CreateShl( mid, 8 );
	
	auto finalAddress = m_IRBuilder.CreateOr( low, m_IRBuilder.CreateOr( midShifted, highShifted ) );
	auto finalAddressPlusY = m_IRBuilder.CreateAdd( finalAddress, m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerY ), llvm::Type::getInt32Ty( m_LLVMContext ) ) );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( finalAddressPlusY );
	PerformLda16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( finalAddressPlusY );
	PerformLda8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformLdaLongIdxX( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );
	
	auto baseAddress = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) );
	auto x = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerX ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto finalAddress = m_IRBuilder.CreateAdd( baseAddress, x );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = DynamicLoad16( finalAddress );
	PerformLda16( m_IRBuilder.CreateLoad( ptr16 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = DynamicLoad8( finalAddress );
	PerformLda8( m_IRBuilder.CreateLoad( ptr8 ) );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}


void Recompiler::PerformLdaLong( const uint32_t address )
{
	auto [ cond, thenBlock, elseBlock, endBlock ] = CreateRegisterWidthTest( M_FLAG );

	m_IRBuilder.CreateCondBr( cond, thenBlock, elseBlock );
	SelectBlock( thenBlock );
	auto ptr16 = StaticLoad16( address );
	PerformLda16( ptr16 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto ptr8 = StaticLoad8( address );
	PerformLda8( ptr8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

llvm::Value* Recompiler::StaticLoad16( const uint32_t address )
{
	auto low = StaticLoad8( address );
	auto high = StaticLoad8( address + 1 );
	auto low16 = m_IRBuilder.CreateZExt( low, llvm::Type::getInt16Ty( m_LLVMContext ), "" );
	auto high16 = m_IRBuilder.CreateZExt( high, llvm::Type::getInt16Ty( m_LLVMContext ), "" );
	auto word = m_IRBuilder.CreateShl( high16, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 8, false ) ), "" );
	return m_IRBuilder.CreateOr( word, low16, "" );
}

llvm::Value* Recompiler::StaticLoad8( const uint32_t address )
{
	auto [ bank, bank_offset ] = getBankAndOffset( address );

	if ( bank <= 0x1f && bank_offset <= 0x1fff )
	{
		return m_IRBuilder.CreateLoad( wRamPtr8( 0x1fff & address ) );
	}
	else if ( ( bank <= 0x3f || ( bank >= 0x80 && bank <= 0xbf ) ) && bank_offset >= 0x2140 && bank_offset <= 0x217F )
	{
		llvm::Value* args[] = { m_IRBuilder.CreateAnd( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( bank_offset ), false ) ), llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x03, false ) ) ) };
		return m_IRBuilder.CreateTrunc( m_IRBuilder.CreateCall( m_SPCReadPortFunction, args ), llvm::Type::getInt8Ty( m_LLVMContext ) );
	}
	else if ( bank <= 0x3f && ( bank_offset >= 0x6000 && bank_offset <= 0x7001 ) )
	{
		llvm::Value* argsDSP[] = { llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( address ), false ) ) };
		return m_IRBuilder.CreateCall( m_DSPReadFunction, argsDSP );
	}
	else if ( bank <= 0x1f && bank_offset >= 0x8000 && bank_offset <= 0xffff )
	{
		return m_IRBuilder.CreateLoad( romPtr8( address ) );
	}
	else if ( bank >= 0x20 && bank <= 0x3f && bank_offset <= 0x1fff )
	{
		return m_IRBuilder.CreateLoad( wRamPtr8( 0x1fff & address ) );
	}
	else if ( bank >= 0x20 && bank <= 0x3f && bank_offset >= 0x8000 && bank_offset <= 0xffff )
	{
		return m_IRBuilder.CreateLoad( romPtr8( address - 0x200000 ) );
	}
	else if ( bank >= 0x40 && bank <= 0x7d && bank_offset <= 0xffff )
	{
		return m_IRBuilder.CreateLoad( romPtr8( address - 0x400000 ) );
	}
	else if ( bank >= 0x7e && bank <= 0x7f && bank_offset <= 0xffff )
	{
		return m_IRBuilder.CreateLoad( wRamPtr8( address - 0x7e0000 ) );
	}
	else if ( bank >= 0xc0 && bank <= 0xfd && bank_offset <= 0xffff )
	{
		return m_IRBuilder.CreateLoad( romPtr8( address - 0xc00000 ) );
	}
	else if ( bank >= 0xfe && bank <= 0xff && bank_offset <= 0xffff )
	{
		return m_IRBuilder.CreateLoad( romPtr8( address - 0xc00000 ) );
	}
	else if ( bank >= 0x80 && bank <= 0x9f && bank_offset <= 0x1fff )
	{
		return m_IRBuilder.CreateLoad( wRamPtr8( 0x1fff & address ) );
	}
	else if ( bank >= 0xa0 && bank <= 0xbf && bank_offset <= 0x1fff )
	{
		return m_IRBuilder.CreateLoad( wRamPtr8( 0x1fff & address ) );
	}

	return llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, static_cast<uint64_t>( 0 ), false ) );
}

void Recompiler::DynamicStore16( llvm::Value* address, llvm::Value* value )
{
	auto low16 = m_IRBuilder.CreateAnd( value, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0xff, false ) ), "" );
	auto low8 = m_IRBuilder.CreateTrunc( low16, llvm::Type::getInt8Ty( m_LLVMContext ), "" );
	DynamicStore8( address, low8 );

	auto high16 = m_IRBuilder.CreateLShr( value, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 8, false ) ), "" );
	auto high8 = m_IRBuilder.CreateTrunc( high16, llvm::Type::getInt8Ty( m_LLVMContext ), "" );
	DynamicStore8( m_IRBuilder.CreateAdd( address, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 1, false ) ) ), high8 );
}

void Recompiler::DynamicStore8( llvm::Value* address, llvm::Value* value )
{
	auto bank = m_IRBuilder.CreateLShr( m_IRBuilder.CreateAnd( address, llvm::APInt( 32, static_cast<uint64_t>( 0xff0000 ), false ) ), 16 );
	auto bank_offset = m_IRBuilder.CreateAnd( address, llvm::APInt( 32, static_cast<uint64_t>( 0xffff ), false ) );

	auto bankCond = m_IRBuilder.CreateICmpULE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x1f, false ) ) );
	auto bankOffsetCond = m_IRBuilder.CreateICmpULE( bank_offset, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x1fff, false ) ) );
	auto thenBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto elseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto endBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	if ( m_CurrentBasicBlock )
	{
		thenBlock->moveAfter( m_CurrentBasicBlock );
		elseBlock->moveAfter( thenBlock );
		endBlock->moveAfter( elseBlock );
	}
	m_IRBuilder.CreateCondBr( m_IRBuilder.CreateAnd( bankCond, bankOffsetCond ), thenBlock, elseBlock );

	SelectBlock( thenBlock );
	m_IRBuilder.CreateStore( value, wRamPtr8( bank_offset ) );
	CreateBranch( endBlock );

	SelectBlock( elseBlock );
	auto thenBlockAPU = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock->getParent() );
	auto elseBlockAPU = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock->getParent() );
	thenBlockAPU->moveAfter( elseBlock );
	elseBlockAPU->moveAfter( thenBlockAPU );
	endBlock->moveAfter( elseBlockAPU );

	auto apuBankCond1 = m_IRBuilder.CreateICmpULE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x3f, false ) ) );
	auto apuBankCond2 = m_IRBuilder.CreateICmpUGE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x80, false ) ) );
	auto apuBankCond3 = m_IRBuilder.CreateICmpULE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0xbf, false ) ) );

	auto apuBankOffsetCondLow = m_IRBuilder.CreateICmpUGE( bank_offset, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x2140, false ) ) );
	auto apuBankOffsetCondHigh = m_IRBuilder.CreateICmpULE( bank_offset, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x217F, false ) ) );

	auto apuBankOffsetInRange = m_IRBuilder.CreateAnd( apuBankOffsetCondLow, apuBankOffsetCondHigh );

	auto apuTest1 = m_IRBuilder.CreateAnd( apuBankCond1, apuBankOffsetInRange );
	auto apuTest2 = m_IRBuilder.CreateAnd( m_IRBuilder.CreateAnd( apuBankCond2, apuBankCond3 ), apuBankOffsetInRange );

	m_IRBuilder.CreateCondBr( m_IRBuilder.CreateOr( apuTest1, apuTest2 ), thenBlockAPU, elseBlockAPU );

	SelectBlock( thenBlockAPU );
	llvm::Value* args[] = { m_IRBuilder.CreateAnd( bank_offset, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x03, false ) ) ),
													m_IRBuilder.CreateZExt( value, llvm::Type::getInt32Ty( m_LLVMContext ) ) };
	m_IRBuilder.CreateCall( m_SPCWritePortFunction, args );
	CreateBranch( endBlock );

	SelectBlock( elseBlockAPU );
	auto thenBlockDSP = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock->getParent() );
	auto elseBlockDSP = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock->getParent() );
	thenBlockDSP->moveAfter( elseBlockAPU );
	elseBlockDSP->moveAfter( thenBlockDSP );
	endBlock->moveAfter( elseBlockDSP );

	auto dspBankCond = m_IRBuilder.CreateICmpULE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x3f, false ) ) );
	auto dspBankOffsetCond1 = m_IRBuilder.CreateICmpUGE( bank_offset, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x6000, false ) ) );
	auto dspBankOffsetCond2 = m_IRBuilder.CreateICmpULE( bank_offset, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x7001, false ) ) );

	auto dspBankOffset = m_IRBuilder.CreateAnd( dspBankOffsetCond1, dspBankOffsetCond2 );
	m_IRBuilder.CreateCondBr( m_IRBuilder.CreateAnd( dspBankCond, dspBankOffset ), thenBlockDSP, elseBlockDSP );

	SelectBlock( thenBlockDSP );
	llvm::Value* argsDSP[] = { address, value };
	m_IRBuilder.CreateCall( m_DSPWriteFunction, argsDSP );
	CreateBranch( endBlock );

	SelectBlock( elseBlockDSP );
	auto thenBlock2 = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock->getParent() );
	auto elseBlock2 = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock->getParent() );
	thenBlock2->moveAfter( elseBlockDSP );
	elseBlock2->moveAfter( thenBlock2 );
	endBlock->moveAfter( elseBlock2 );
	bankCond = m_IRBuilder.CreateICmpUGE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x20, false ) ) );
	auto bankCond2 = m_IRBuilder.CreateICmpULE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x3f, false ) ) );
	bankOffsetCond = m_IRBuilder.CreateICmpULE( bank_offset, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x1fff, false ) ) );
	m_IRBuilder.CreateCondBr( m_IRBuilder.CreateAnd( bankCond, m_IRBuilder.CreateAnd( bankCond2, bankOffsetCond ) ), thenBlock2, elseBlock2 );
	SelectBlock( thenBlock2 );
	m_IRBuilder.CreateStore( value, wRamPtr8( m_IRBuilder.CreateAnd( address, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x1fff, false ) ) ) ) );
	CreateBranch( endBlock );

	SelectBlock( elseBlock2 );
	auto thenBlock3 = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock->getParent() );
	auto elseBlock3 = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock->getParent() );
	thenBlock3->moveAfter( elseBlock2 );
	elseBlock3->moveAfter( thenBlock3 );
	endBlock->moveAfter( elseBlock3 );
	bankCond = m_IRBuilder.CreateICmpUGE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x7e, false ) ) );
	bankCond2 = m_IRBuilder.CreateICmpULE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x7f, false ) ) );
	auto bankOffsetCond2 = m_IRBuilder.CreateICmpULE( bank_offset, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0xffff, false ) ) );
	m_IRBuilder.CreateCondBr( m_IRBuilder.CreateAnd( bankCond, m_IRBuilder.CreateAnd( bankCond2, bankOffsetCond2 ) ), thenBlock3, elseBlock3 );
	SelectBlock( thenBlock3 );
	m_IRBuilder.CreateStore( value, wRamPtr8( m_IRBuilder.CreateSub( address, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x7e0000, false ) ) ) ) );
	CreateBranch( endBlock );

	SelectBlock( elseBlock3 );
	auto thenBlock4 = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock->getParent() );
	auto elseBlock4 = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock->getParent() );
	thenBlock4->moveAfter( elseBlock3 );
	elseBlock4->moveAfter( thenBlock4 );
	endBlock->moveAfter( elseBlock4 );
	bankCond = m_IRBuilder.CreateICmpUGE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x80, false ) ) );
	bankCond2 = m_IRBuilder.CreateICmpULE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x9f, false ) ) );
	bankOffsetCond2 = m_IRBuilder.CreateICmpULE( bank_offset, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x1fff, false ) ) );
	m_IRBuilder.CreateCondBr( m_IRBuilder.CreateAnd( bankCond, m_IRBuilder.CreateAnd( bankCond2, bankOffsetCond2 ) ), thenBlock4, elseBlock4 );
	SelectBlock( thenBlock4 );
	m_IRBuilder.CreateStore( value, wRamPtr8( m_IRBuilder.CreateAnd( address, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x1fff, false ) ) ) ) );
	CreateBranch( endBlock );

	SelectBlock( elseBlock4 );
	auto thenBlock5 = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock->getParent() );
	thenBlock5->moveAfter( elseBlock4 );
	endBlock->moveAfter( thenBlock5 );
	bankCond = m_IRBuilder.CreateICmpUGE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0xa0, false ) ) );
	bankCond2 = m_IRBuilder.CreateICmpULE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0xbf, false ) ) );
	bankOffsetCond2 = m_IRBuilder.CreateICmpULE( bank_offset, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x1fff, false ) ) );
	m_IRBuilder.CreateCondBr( m_IRBuilder.CreateAnd( bankCond, m_IRBuilder.CreateAnd( bankCond2, bankOffsetCond2 ) ), thenBlock5, endBlock );
	SelectBlock( thenBlock5 );
	m_IRBuilder.CreateStore( value, wRamPtr8( m_IRBuilder.CreateAnd( address, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x1fff, false ) ) ) ) );
	CreateBranch( endBlock );

	SelectBlock( endBlock );
}

llvm::Value* Recompiler::DynamicLoad16( llvm::Value* address )
{
	auto low = m_IRBuilder.CreateLoad( DynamicLoad8( address ) );
	auto high = m_IRBuilder.CreateLoad( DynamicLoad8( m_IRBuilder.CreateAdd( address, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 1, false ) ) ) ) );
	auto low16 = m_IRBuilder.CreateZExt( low, llvm::Type::getInt16Ty( m_LLVMContext ), "" );
	auto high16 = m_IRBuilder.CreateZExt( high, llvm::Type::getInt16Ty( m_LLVMContext ), "" );
	auto word = m_IRBuilder.CreateShl( high16, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 8, false ) ), "" );

	auto result = &m_DynamicLoad16;
	m_IRBuilder.CreateStore( m_IRBuilder.CreateOr( word, low16, "" ), result );

	return result;
}

llvm::Value* Recompiler::DynamicLoad8( llvm::Value* address )
{
	auto result = &m_DynamicLoad8;
	
	auto bank = m_IRBuilder.CreateLShr( m_IRBuilder.CreateAnd( address, llvm::APInt( 32, static_cast<uint64_t>( 0xff0000 ), false ) ), 16 );
	auto bank_offset = m_IRBuilder.CreateAnd( address, llvm::APInt( 32, static_cast<uint64_t>( 0xffff ), false ) );

	auto bankCond = m_IRBuilder.CreateICmpULE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x1f, false ) ) );
	auto bankOffsetCond = m_IRBuilder.CreateICmpULE( bank_offset, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x1fff, false ) ) );
	auto thenBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto elseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto endBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	if ( m_CurrentBasicBlock )
	{
		thenBlock->moveAfter( m_CurrentBasicBlock );
		elseBlock->moveAfter( thenBlock );
		endBlock->moveAfter( elseBlock );
	}
	m_IRBuilder.CreateCondBr( m_IRBuilder.CreateAnd( bankCond, bankOffsetCond ), thenBlock, elseBlock );

	SelectBlock( thenBlock );
	m_IRBuilder.CreateStore( m_IRBuilder.CreateLoad( wRamPtr8( bank_offset ) ), result );
	CreateBranch( endBlock );

	SelectBlock( elseBlock );
	auto thenBlockAPU = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock->getParent() );
	auto elseBlockAPU = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock->getParent() );
	thenBlockAPU->moveAfter( elseBlock );
	elseBlockAPU->moveAfter( thenBlockAPU );
	endBlock->moveAfter( elseBlockAPU );

	auto apuBankCond1 = m_IRBuilder.CreateICmpULE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x3f, false ) ) );
	auto apuBankCond2 = m_IRBuilder.CreateICmpUGE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x80, false ) ) );
	auto apuBankCond3 = m_IRBuilder.CreateICmpULE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0xbf, false ) ) );

	auto apuBankOffsetCondLow = m_IRBuilder.CreateICmpUGE( bank_offset, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x2140, false ) ) );
	auto apuBankOffsetCondHigh = m_IRBuilder.CreateICmpULE( bank_offset, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x217F, false ) ) );

	auto apuBankOffsetInRange = m_IRBuilder.CreateAnd( apuBankOffsetCondLow, apuBankOffsetCondHigh );

	auto apuTest1 = m_IRBuilder.CreateAnd( apuBankCond1, apuBankOffsetInRange );
	auto apuTest2 = m_IRBuilder.CreateAnd( m_IRBuilder.CreateAnd( apuBankCond2, apuBankCond3 ), apuBankOffsetInRange );

	m_IRBuilder.CreateCondBr( m_IRBuilder.CreateOr( apuTest1, apuTest2 ), thenBlockAPU, elseBlockAPU );

	SelectBlock( thenBlockAPU );
	llvm::Value* args[] = { m_IRBuilder.CreateAnd( bank_offset, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x03, false ) ) ) };
	m_IRBuilder.CreateStore( m_IRBuilder.CreateTrunc( m_IRBuilder.CreateCall( m_SPCReadPortFunction, args ), llvm::Type::getInt8Ty( m_LLVMContext ) ), result );
	CreateBranch( endBlock );

	SelectBlock( elseBlockAPU );

	auto thenBlockDSP = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock->getParent() );
	auto elseBlockDSP = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock->getParent() );
	thenBlockDSP->moveAfter( elseBlockAPU );
	elseBlockDSP->moveAfter( thenBlockDSP );
	endBlock->moveAfter( elseBlockDSP );

	auto dspBankCond = m_IRBuilder.CreateICmpULE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x3f, false ) ) );
	auto dspBankOffsetCond1 = m_IRBuilder.CreateICmpUGE( bank_offset, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x6000, false ) ) );
	auto dspBankOffsetCond2 = m_IRBuilder.CreateICmpULE( bank_offset, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x7001, false ) ) );

	auto dspBankOffset = m_IRBuilder.CreateAnd( dspBankOffsetCond1, dspBankOffsetCond2 );
	m_IRBuilder.CreateCondBr( m_IRBuilder.CreateAnd( dspBankCond, dspBankOffset ), thenBlockDSP, elseBlockDSP );

	SelectBlock( thenBlockDSP );
	llvm::Value* argsDSP[] = { address };
	m_IRBuilder.CreateStore( m_IRBuilder.CreateCall( m_DSPReadFunction, argsDSP ), result );
	CreateBranch( endBlock );

	SelectBlock( elseBlockDSP );
	auto thenBlock2 = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock->getParent() );
	auto elseBlock2 = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock->getParent() );
	thenBlock2->moveAfter( elseBlockDSP );
	elseBlock2->moveAfter( thenBlock2 );
	endBlock->moveAfter( elseBlock2 );
	bankCond = m_IRBuilder.CreateICmpULE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x1f, false ) ) );
	bankOffsetCond = m_IRBuilder.CreateICmpUGE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x8000, false ) ) );
	auto bankOffsetCond2 = m_IRBuilder.CreateICmpULE( bank_offset, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0xffff, false ) ) );
	m_IRBuilder.CreateCondBr( m_IRBuilder.CreateAnd( bankCond, m_IRBuilder.CreateAnd( bankOffsetCond, bankOffsetCond2 ) ), thenBlock2, elseBlock2 );
	SelectBlock( thenBlock2 );
	m_IRBuilder.CreateStore( m_IRBuilder.CreateLoad( romPtr8( address ) ), result );
	CreateBranch( endBlock );

	SelectBlock( elseBlock2 );
	auto thenBlock3 = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock2->getParent() );
	auto elseBlock3 = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock2->getParent() );
	thenBlock3->moveAfter( elseBlock2 );
	elseBlock3->moveAfter( thenBlock3 );
	endBlock->moveAfter( elseBlock3 );
	bankCond = m_IRBuilder.CreateICmpUGE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x20, false ) ) );
	auto bankCond2 = m_IRBuilder.CreateICmpULE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x3f, false ) ) );
	bankOffsetCond = m_IRBuilder.CreateICmpULE( bank_offset, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x1fff, false ) ) );
	m_IRBuilder.CreateCondBr( m_IRBuilder.CreateAnd( bankCond, m_IRBuilder.CreateAnd( bankCond2, bankOffsetCond ) ), thenBlock3, elseBlock3 );
	SelectBlock( thenBlock3 );
	m_IRBuilder.CreateStore( m_IRBuilder.CreateLoad( wRamPtr8( m_IRBuilder.CreateAnd( address, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x1fff, false ) ) ) ) ), result );
	CreateBranch( endBlock );

	SelectBlock( elseBlock3 );
	auto thenBlock4 = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock3->getParent() );
	auto elseBlock4 = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock3->getParent() );
	thenBlock4->moveAfter( elseBlock3 );
	elseBlock4->moveAfter( thenBlock4 );
	endBlock->moveAfter( elseBlock4 );
	bankCond = m_IRBuilder.CreateICmpUGE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x20, false ) ) );
	bankCond2 = m_IRBuilder.CreateICmpULE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x3f, false ) ) );
	bankOffsetCond = m_IRBuilder.CreateICmpUGE( bank_offset, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x8000, false ) ) );
	bankOffsetCond2 = m_IRBuilder.CreateICmpULE( bank_offset, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0xffff, false ) ) );
	m_IRBuilder.CreateCondBr( m_IRBuilder.CreateAnd( bankCond, m_IRBuilder.CreateAnd( bankCond2, m_IRBuilder.CreateAnd( bankOffsetCond, bankOffsetCond2 ) ) ), thenBlock4, elseBlock4 );
	SelectBlock( thenBlock4 );
	m_IRBuilder.CreateStore( m_IRBuilder.CreateLoad( romPtr8( m_IRBuilder.CreateSub( address, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x200000, false ) ) ) ) ), result );
	CreateBranch( endBlock );

	SelectBlock( elseBlock4 );
	auto thenBlock5 = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock4->getParent() );
	auto elseBlock5 = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock4->getParent() );
	thenBlock5->moveAfter( elseBlock4 );
	elseBlock5->moveAfter( thenBlock5 );
	endBlock->moveAfter( elseBlock5 );
	bankCond = m_IRBuilder.CreateICmpUGE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x40, false ) ) );
	bankCond2 = m_IRBuilder.CreateICmpULE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x7d, false ) ) );
	bankOffsetCond2 = m_IRBuilder.CreateICmpULE( bank_offset, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0xffff, false ) ) );
	m_IRBuilder.CreateCondBr( m_IRBuilder.CreateAnd( bankCond, m_IRBuilder.CreateAnd( bankCond2, bankOffsetCond2 ) ), thenBlock5, elseBlock5 );
	SelectBlock( thenBlock5 );
	m_IRBuilder.CreateStore( m_IRBuilder.CreateLoad( romPtr8( m_IRBuilder.CreateSub( address, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x400000, false ) ) ) ) ), result );
	CreateBranch( endBlock );

	SelectBlock( elseBlock5 );
	auto thenBlock6 = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock5->getParent() );
	auto elseBlock6 = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock5->getParent() );
	thenBlock6->moveAfter( elseBlock5 );
	elseBlock6->moveAfter( thenBlock6 );
	endBlock->moveAfter( elseBlock6 );
	bankCond = m_IRBuilder.CreateICmpUGE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x7e, false ) ) );
	bankCond2 = m_IRBuilder.CreateICmpULE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x7f, false ) ) );
	bankOffsetCond2 = m_IRBuilder.CreateICmpULE( bank_offset, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0xffff, false ) ) );
	m_IRBuilder.CreateCondBr( m_IRBuilder.CreateAnd( bankCond, m_IRBuilder.CreateAnd( bankCond2, bankOffsetCond2 ) ), thenBlock6, elseBlock6 );
	SelectBlock( thenBlock6 );
	m_IRBuilder.CreateStore( m_IRBuilder.CreateLoad( wRamPtr8( m_IRBuilder.CreateSub( address, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x7e0000, false ) ) ) ) ), result );
	CreateBranch( endBlock );

	SelectBlock( elseBlock6 );
	auto thenBlock7 = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock6->getParent() );
	auto elseBlock7 = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock6->getParent() );
	thenBlock7->moveAfter( elseBlock6 );
	elseBlock7->moveAfter( thenBlock7 );
	endBlock->moveAfter( elseBlock7 );
	bankCond = m_IRBuilder.CreateICmpUGE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0xc0, false ) ) );
	bankCond2 = m_IRBuilder.CreateICmpULE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0xfd, false ) ) );
	bankOffsetCond2 = m_IRBuilder.CreateICmpULE( bank_offset, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0xffff, false ) ) );
	m_IRBuilder.CreateCondBr( m_IRBuilder.CreateAnd( bankCond, m_IRBuilder.CreateAnd( bankCond2, bankOffsetCond2 ) ), thenBlock7, elseBlock7 );
	SelectBlock( thenBlock7 );
	m_IRBuilder.CreateStore( m_IRBuilder.CreateLoad( romPtr8( m_IRBuilder.CreateSub( address, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0xc00000, false ) ) ) ) ), result );
	CreateBranch( endBlock );

	SelectBlock( elseBlock7 );
	auto thenBlock8 = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock7->getParent() );
	auto elseBlock8 = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock6->getParent() );
	thenBlock8->moveAfter( elseBlock7 );
	elseBlock8->moveAfter( thenBlock8 );
	endBlock->moveAfter( thenBlock8 );
	bankCond = m_IRBuilder.CreateICmpUGE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0xfe, false ) ) );
	bankCond2 = m_IRBuilder.CreateICmpULE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0xff, false ) ) );
	bankOffsetCond2 = m_IRBuilder.CreateICmpULE( bank_offset, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0xffff, false ) ) );
	m_IRBuilder.CreateCondBr( m_IRBuilder.CreateAnd( bankCond, m_IRBuilder.CreateAnd( bankCond2, bankOffsetCond2 ) ), thenBlock8, elseBlock8 );
	SelectBlock( thenBlock8 );
	m_IRBuilder.CreateStore( m_IRBuilder.CreateLoad( romPtr8( m_IRBuilder.CreateSub( address, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0xc00000, false ) ) ) ) ), result );
	CreateBranch( endBlock );

	SelectBlock( elseBlock8 );
	auto thenBlock9 = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock7->getParent() );
	auto elseBlock9 = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock6->getParent() );
	thenBlock9->moveAfter( elseBlock8 );
	elseBlock9->moveAfter( thenBlock9 );
	endBlock->moveAfter( thenBlock9 );
	bankCond = m_IRBuilder.CreateICmpUGE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x80, false ) ) );
	bankCond2 = m_IRBuilder.CreateICmpULE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x9f, false ) ) );
	bankOffsetCond2 = m_IRBuilder.CreateICmpULE( bank_offset, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x1fff, false ) ) );
	m_IRBuilder.CreateCondBr( m_IRBuilder.CreateAnd( bankCond, m_IRBuilder.CreateAnd( bankCond2, bankOffsetCond2 ) ), thenBlock9, elseBlock9 );
	SelectBlock( thenBlock9 );
	m_IRBuilder.CreateStore( m_IRBuilder.CreateLoad( wRamPtr8( m_IRBuilder.CreateAnd( address, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x1fff, false ) ) ) ) ), result );
	CreateBranch( endBlock );

	SelectBlock( elseBlock9 );
	auto thenBlock10 = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock8->getParent() );
	auto elseBlock10 = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock8->getParent() );
	thenBlock10->moveAfter( elseBlock9 );
	elseBlock10->moveAfter( thenBlock10 );
	endBlock->moveAfter( elseBlock10 );
	bankCond = m_IRBuilder.CreateICmpUGE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x80, false ) ) );
	bankCond2 = m_IRBuilder.CreateICmpULE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x9f, false ) ) );
	bankOffsetCond = m_IRBuilder.CreateICmpUGE( bank_offset, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x8000, false ) ) );
	bankOffsetCond2 = m_IRBuilder.CreateICmpULE( bank_offset, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0xffff, false ) ) );
	m_IRBuilder.CreateCondBr( m_IRBuilder.CreateAnd( bankCond, m_IRBuilder.CreateAnd( bankCond2, m_IRBuilder.CreateAnd( bankOffsetCond, bankOffsetCond2 ) ) ), thenBlock10, elseBlock10 );
	SelectBlock( thenBlock10 );
	m_IRBuilder.CreateStore( m_IRBuilder.CreateLoad( romPtr8( m_IRBuilder.CreateSub( address, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x800000, false ) ) ) ) ), result );
	CreateBranch( endBlock );

	SelectBlock( elseBlock10 );
	auto thenBlock11 = llvm::BasicBlock::Create( m_LLVMContext, "", elseBlock7->getParent() );
	thenBlock11->moveAfter( elseBlock10 );
	endBlock->moveAfter( thenBlock11 );
	bankCond = m_IRBuilder.CreateICmpUGE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0xa0, false ) ) );
	bankCond2 = m_IRBuilder.CreateICmpULE( bank, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0xbf, false ) ) );
	bankOffsetCond2 = m_IRBuilder.CreateICmpULE( bank_offset, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x1fff, false ) ) );
	m_IRBuilder.CreateCondBr( m_IRBuilder.CreateAnd( bankCond, m_IRBuilder.CreateAnd( bankCond2, bankOffsetCond2 ) ), thenBlock11, endBlock );
	SelectBlock( thenBlock11 );
	m_IRBuilder.CreateStore( m_IRBuilder.CreateLoad( wRamPtr8( m_IRBuilder.CreateAnd( address, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0x1fff, false ) ) ) ) ), result );
	CreateBranch( endBlock );

	SelectBlock( endBlock );

	return result;
}

void Recompiler::LoadAST( const char* filename )
{
	std::ifstream ifs( filename );
	if ( ifs.is_open() )
	{
		auto j = nlohmann::json::parse( ifs );

		std::vector<nlohmann::json> ast;
		j[ "rom_reset_func_name" ].get_to( m_RomResetFuncName );
		j[ "rom_reset_addr" ].get_to( m_RomResetAddr );
		j[ "rom_nmi_func_name" ].get_to( m_RomNmiFuncName );
		j[ "rom_irq_func_name" ].get_to( m_RomIrqFuncName );
		j[ "ast" ].get_to( ast );
		j[ "offset_to_function_name" ].get_to( m_OffsetToFunctionName );
		j[ "jump_tables" ].get_to( m_JumpTables );
		j[ "function_names" ].get_to( m_FunctionNames );
		j[ "labels_to_functions" ].get_to( m_LabelsToFunctions );

		const auto numNodes = ast.size();
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
						m_Program.emplace_back( Instruction{ current_node[ "Instruction" ][ "offset" ], current_node[ "Instruction" ][ "instruction_string" ], current_node[ "Instruction" ][ "opcode" ], current_node[ "Instruction" ][ "operand" ], current_node[ "Instruction" ][ "jump_label_name" ],  current_node[ "Instruction" ][ "operand_size" ],  current_node[ "Instruction" ][ "memory_mode" ], current_node[ "Instruction" ][ "index_mode" ], current_node[ "Instruction" ][ "func_names" ] } );
					}
					else
					{
						m_Program.emplace_back( Instruction{ current_node[ "Instruction" ][ "offset" ], current_node[ "Instruction" ][ "instruction_string" ], current_node[ "Instruction" ][ "opcode" ], current_node[ "Instruction" ][ "operand" ], current_node[ "Instruction" ][ "operand_size" ],  current_node[ "Instruction" ][ "memory_mode" ], current_node[ "Instruction" ][ "index_mode" ], current_node[ "Instruction" ][ "func_names" ] } );
					}
				}
				else
				{
					m_Program.emplace_back( Instruction{ current_node[ "Instruction" ][ "offset" ], current_node[ "Instruction" ][ "instruction_string" ], current_node[ "Instruction" ][ "opcode" ], current_node[ "Instruction" ][ "memory_mode" ], current_node[ "Instruction" ][ "index_mode" ], current_node[ "Instruction" ][ "func_names" ] } );
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

Recompiler::Instruction::Instruction( const uint32_t offset, const std::string& instructionString, const uint8_t opcode, const uint32_t operand, const uint32_t operand_size, MemoryMode memoryMode, MemoryMode indexMode, const std::set<std::string>& funcNames )
	: m_Offset( offset )
	, m_InstructionString( instructionString )
	, m_Opcode( opcode )
	, m_Operand( operand )
	, m_OperandSize( operand_size )
	, m_MemoryMode( memoryMode )
	, m_IndexMode( indexMode )
	, m_HasOperand( true )
	, m_FuncNames( funcNames )
{
}

Recompiler::Instruction::Instruction( const uint32_t offset, const std::string& instructionString, const uint8_t opcode, const uint32_t operand, const std::string& jumpLabelName, const uint32_t operand_size, MemoryMode memoryMode, MemoryMode indexMode, const std::set<std::string>& funcNames )
	: m_Offset( offset )
	, m_InstructionString( instructionString )
	, m_Opcode( opcode )
	, m_Operand( operand )
	, m_JumpLabelName( jumpLabelName )
	, m_OperandSize( operand_size )
	, m_MemoryMode( memoryMode )
	, m_IndexMode( indexMode )
	, m_HasOperand( true )
	, m_FuncNames( funcNames )
{
}

Recompiler::Instruction::Instruction( const uint32_t offset, const std::string& instructionString, const uint8_t opcode, MemoryMode memoryMode, MemoryMode indexMode, const std::set<std::string>& funcNames )
	: m_Offset( offset )
	, m_InstructionString( instructionString )
	, m_Opcode( opcode )
	, m_Operand( 0 )
	, m_OperandSize( 0 )
	, m_MemoryMode( memoryMode )
	, m_IndexMode( indexMode )
	, m_HasOperand( false )
	, m_FuncNames( funcNames )
{
}

Recompiler::Instruction::~Instruction()
{
}
