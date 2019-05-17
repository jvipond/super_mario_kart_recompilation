#include "Recompiler.hpp"
#include "json.hpp"
#include <fstream>
#include <iostream>
#include <iomanip>
#include "llvm/Support/TargetSelect.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Type.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

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
, m_registerStatusBreak( m_RecompilationModule, llvm::Type::getInt1Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "StatusBreak" )
, m_registerStatusCarry( m_RecompilationModule, llvm::Type::getInt1Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "StatusCarry" )
, m_registerStatusDecimal( m_RecompilationModule, llvm::Type::getInt1Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "StatusDecimal" )
, m_registerStatusInterrupt( m_RecompilationModule, llvm::Type::getInt1Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "StatusInterrupt" )
, m_registerStatusMemoryWidth( m_RecompilationModule, llvm::Type::getInt1Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "StatusMemoryWidth" )
, m_registerStatusNegative( m_RecompilationModule, llvm::Type::getInt1Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "StatusNegative" )
, m_registerStatusOverflow( m_RecompilationModule, llvm::Type::getInt1Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "StatusOverflow" )
, m_registerStatusIndexWidth( m_RecompilationModule, llvm::Type::getInt1Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "StatusIndexWidth" )
, m_registerStatusZero( m_RecompilationModule, llvm::Type::getInt1Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "StatusZero" )
, m_CurrentBasicBlock( nullptr )
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
	m_registerStatusBreak.removeFromParent();
	m_registerStatusCarry.removeFromParent();
	m_registerStatusDecimal.removeFromParent();
	m_registerStatusInterrupt.removeFromParent();
	m_registerStatusMemoryWidth.removeFromParent();
	m_registerStatusNegative.removeFromParent();
	m_registerStatusOverflow.removeFromParent();
	m_registerStatusIndexWidth.removeFromParent();
	m_registerStatusZero.removeFromParent();
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

	auto pc = m_IRBuilder.CreateLoad( &m_registerPC, "" );
	auto sw = m_IRBuilder.CreateSwitch( pc, dynamicJumpTableDefaultCaseBlock, static_cast<unsigned int>( m_LabelNamesToOffsets.size() ) );
	for ( auto&& entry : m_LabelNamesToOffsets )
	{
		auto addressValue = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( entry.second ), false ) );
		if ( sw )
		{
			sw->addCase( addressValue, m_LabelNamesToBasicBlocks[ entry.first ] );
		}
	}
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
			case 0x08:
				break;
			case 0x09: // ORA immediate
			{
				llvm::Value* operandValue = llvm::ConstantInt::get( m_Context, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
				m_Recompiler.PerformOra( operandValue );
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
			}
			break;
			case 0x18: // CLC implied
			{
				m_Recompiler.ClearCarry();
			}
			break;
			case 0x38: // SEC implied
			{
				m_Recompiler.SetCarry();
			}
			break;
			case 0xD8: // CLD implied
			{
				m_Recompiler.ClearDecimal();
			}
			break;
			case 0x58: // CLI implied
			{
				m_Recompiler.ClearInterrupt();
			}
			break;
			case 0xB8: // CLV implied
			{
				m_Recompiler.ClearOverflow();
			}
			break;
			case 0xF8: // SED implied
			{
				m_Recompiler.SetDecimal();
			}
			break;
			case 0x78: // SEI implied
			{
				m_Recompiler.SetInterrupt();
			}
			break;
			case 0xEB: // XBA
			{
				m_Recompiler.PerformXba();
			}
			break;
			case 0x1B: // TCS
			{
				m_Recompiler.PerformTcs();
			}
			break;
			case 0x5B: // TCD
			{
				m_Recompiler.PerformTcd();
			}
			break;
			case 0x7B: // TDC
			{
				m_Recompiler.PerformTdc();
			}
			break;
			case 0x3B: // TSC
			{
				m_Recompiler.PerformTsc();
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

	// Create a main function and add an entry block:
	llvm::FunctionType* mainFunctionType = llvm::FunctionType::get( llvm::Type::getInt32Ty( m_LLVMContext ), false );
	m_MainFunction = llvm::Function::Create( mainFunctionType, llvm::Function::PrivateLinkage, "main", m_RecompilationModule );
	llvm::BasicBlock* entry = llvm::BasicBlock::Create( m_LLVMContext, "EntryBlock", m_MainFunction );
	m_IRBuilder.SetInsertPoint( entry );

	InitialiseBasicBlocksFromLabelNames();
	AddDynamicJumpTableBlock();
	GenerateCode();

	m_IRBuilder.SetInsertPoint( entry, entry->begin() );
	CreateBranch( m_LabelNamesToBasicBlocks[ m_RomResetLabelName ] );

	m_IRBuilder.SetInsertPoint( m_DynamicJumpTableBlock );
	// Add return value of 0:
	llvm::Value* returnValue = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 0, true ) );
	m_IRBuilder.CreateRet( returnValue );

	std::error_code EC;
	llvm::raw_fd_ostream outputHumanReadable( "smk.human_readable.bc", EC );
	m_RecompilationModule.print( outputHumanReadable, nullptr );

	llvm::raw_fd_ostream outputStream( "smk.bc", EC );
	llvm::WriteBitcodeToFile( m_RecompilationModule, outputStream );
	outputStream.flush();
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

void Recompiler::PerformTcs()
{
	m_IRBuilder.CreateStore( &m_registerA, &m_registerSP );
	TestAndSetZero16( &m_registerA );
	TestAndSetNegative16( &m_registerA );
}

void Recompiler::PerformTcd()
{
	m_IRBuilder.CreateStore( &m_registerA, &m_registerDP );
	TestAndSetZero16( &m_registerA );
	TestAndSetNegative16( &m_registerA );
}

void Recompiler::PerformTdc()
{
	m_IRBuilder.CreateStore( &m_registerDP, &m_registerA );
	TestAndSetZero16( &m_registerDP );
	TestAndSetNegative16( &m_registerDP );
}

void Recompiler::PerformTsc()
{
	m_IRBuilder.CreateStore( &m_registerSP, &m_registerA );
	TestAndSetZero16( &m_registerSP );
	TestAndSetNegative16( &m_registerSP );
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
	llvm::Value* v = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 1, static_cast<uint64_t>( 0 ), false ) );
	m_IRBuilder.CreateStore( v, &m_registerStatusCarry );
}

void Recompiler::SetCarry()
{
	llvm::Value* v = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 1, static_cast<uint64_t>( 1 ), false ) );
	m_IRBuilder.CreateStore( v, &m_registerStatusCarry );
}

void Recompiler::ClearDecimal()
{
	llvm::Value* v = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 1, static_cast<uint64_t>( 0 ), false ) );
	m_IRBuilder.CreateStore( v, &m_registerStatusDecimal );
}

void Recompiler::SetDecimal()
{
	llvm::Value* v = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 1, static_cast<uint64_t>( 1 ), false ) );
	m_IRBuilder.CreateStore( v, &m_registerStatusDecimal );
}

void Recompiler::ClearInterrupt()
{
	llvm::Value* v = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 1, static_cast<uint64_t>( 0 ), false ) );
	m_IRBuilder.CreateStore( v, &m_registerStatusInterrupt );
}

void Recompiler::SetInterrupt()
{
	llvm::Value* v = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 1, static_cast<uint64_t>( 1 ), false ) );
	m_IRBuilder.CreateStore( v, &m_registerStatusInterrupt );
}

void Recompiler::ClearOverflow()
{
	llvm::Value* v = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 1, static_cast<uint64_t>( 0 ), false ) );
	m_IRBuilder.CreateStore( v, &m_registerStatusOverflow );
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
	m_IRBuilder.CreateStore( isCarry, &m_registerStatusCarry );
}

void Recompiler::PerformLdy16( llvm::Value* value )
{
	m_IRBuilder.CreateStore( value, &m_registerX );
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
	TestAndSetZero8( writeRegisterA8Bit );
	TestAndSetNegative8( writeRegisterA8Bit );
}

void Recompiler::PerformOra( llvm::Value* value )
{
	llvm::LoadInst* loadA = m_IRBuilder.CreateLoad( &m_registerA, "" );
	llvm::Value* newA = m_IRBuilder.CreateOr( loadA, value, "" );
	m_IRBuilder.CreateStore( newA, &m_registerA );
	TestAndSetZero16( newA );
	TestAndSetNegative16( newA );
}

void Recompiler::TestAndSetZero16( llvm::Value* value )
{
	llvm::Value* zeroConst = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0, false ) );
	llvm::Value* isZero = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_EQ, value, zeroConst, "" );
	m_IRBuilder.CreateStore( isZero, &m_registerStatusZero );
}

void Recompiler::TestAndSetZero8( llvm::Value* value )
{
	llvm::Value* zeroConst = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0, false ) );
	llvm::Value* isZero = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_EQ, value, zeroConst, "" );
	m_IRBuilder.CreateStore( isZero, &m_registerStatusZero );
}

void Recompiler::TestAndSetNegative16( llvm::Value* value )
{
	llvm::Value* x8000 = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0x8000, false ) );
	llvm::Value* masked = m_IRBuilder.CreateAnd( value, x8000, "" );
	llvm::Value* isNeg = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_EQ, masked, x8000, "" );
	m_IRBuilder.CreateStore( isNeg, &m_registerStatusNegative );
}

void Recompiler::TestAndSetNegative8( llvm::Value* value )
{
	llvm::Value* x80 = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 8, 0x80, false ) );
	llvm::Value* masked = m_IRBuilder.CreateAnd( value, x80, "" );
	llvm::Value* isNeg = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_EQ, masked, x80, "" );
	m_IRBuilder.CreateStore( isNeg, &m_registerStatusNegative );
}

void Recompiler::LoadAST( const char* filename )
{
	std::ifstream ifs( filename );
	if ( ifs.is_open() )
	{
		nlohmann::json j = nlohmann::json::parse( ifs );

		std::vector<nlohmann::json> ast;
		j[ "rom_reset_label_name" ].get_to( m_RomResetLabelName );
		j[ "ast" ].get_to( ast );

		for ( auto&& node : ast )
		{
			if ( node.contains( "Label" ) )
			{
				m_Program.emplace_back( Label{ node[ "Label" ][ "name" ], node[ "Label" ][ "offset" ] } );
				m_LabelNamesToOffsets.emplace( node[ "Label" ][ "name" ], node[ "Label" ][ "offset" ] );
			}
			else if ( node.contains( "Instruction" ) )
			{
				if ( node[ "Instruction" ].contains( "operand" ) )
				{
					m_Program.emplace_back( Instruction{ node[ "Instruction" ][ "offset" ], node[ "Instruction" ][ "opcode" ], node[ "Instruction" ][ "operand" ], node[ "Instruction" ][ "operand_size" ],  node[ "Instruction" ][ "memory_mode" ], node[ "Instruction" ][ "index_mode" ] } );
				}
				else
				{
					m_Program.emplace_back( Instruction{ node[ "Instruction" ][ "offset" ], node[ "Instruction" ][ "opcode" ], node[ "Instruction" ][ "memory_mode" ], node[ "Instruction" ][ "index_mode" ] } );
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
