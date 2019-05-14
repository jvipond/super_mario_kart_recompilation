#include "Recompiler.hpp"
#include "json.hpp"
#include <fstream>
#include <iostream>
#include <iomanip>
#include "llvm/Support/TargetSelect.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Type.h"
#include "llvm/Bitcode/BitcodeWriter.h"

Recompiler::Recompiler()
: m_IRBuilder( m_LLVMContext )
, m_RecompilationModule( "recompilation", m_LLVMContext )
, m_DynamicJumpTableBlock( nullptr )
, m_MainFunction( nullptr )
, m_registerA( m_RecompilationModule, llvm::Type::getInt16Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "A" )
, m_registerDB( m_RecompilationModule, llvm::Type::getInt8Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "DB" )
, m_registerDP( m_RecompilationModule, llvm::Type::getInt16Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "DP" )
, m_registerPB( m_RecompilationModule, llvm::Type::getInt8Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "P" )
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
				break;
			case 0x0D: // ORA absolute
			{
				llvm::Value* operandValue = llvm::ConstantInt::get( m_Context, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
				m_Recompiler.PerformOra( operandValue );
			}
			break;
			case 0xAD: // LDA absolute
			{
				llvm::Value* operandValue = llvm::ConstantInt::get( m_Context, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
				m_Recompiler.PerformLda( operandValue );
			}
			break;
			case 0xAE: // LDX absolute
			{
				llvm::Value* operandValue = llvm::ConstantInt::get( m_Context, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
				m_Recompiler.PerformLdx( operandValue );
			}
			break;
			case 0xAC: // LDY absolute
			{
				llvm::Value* operandValue = llvm::ConstantInt::get( m_Context, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
				m_Recompiler.PerformLdy( operandValue );
			}
			break;
			case 0xCD: // CMP absolute
			{
				llvm::Value* lValue = m_Recompiler.CreateLoadA();
				llvm::Value* rValue = llvm::ConstantInt::get( m_Context, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
				m_Recompiler.PerformCmp( lValue, rValue );
			}
			break;
			case 0xEC: // CPX absolute
			{
				llvm::Value* lValue = m_Recompiler.CreateLoadX();
				llvm::Value* rValue = llvm::ConstantInt::get( m_Context, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
				m_Recompiler.PerformCmp( lValue, rValue );
			}
			break;
			case 0xCC: // CPY absolute
			{
				llvm::Value* lValue = m_Recompiler.CreateLoadY();
				llvm::Value* rValue = llvm::ConstantInt::get( m_Context, llvm::APInt( 16, static_cast<uint64_t>( instruction.GetOperand() ), true ) );
				m_Recompiler.PerformCmp( lValue, rValue );
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

void Recompiler::AddLabelNameToBasicBlock( const std::string& labelName, llvm::BasicBlock* basicBlock )
{
	m_LabelNamesToBasicBlocks.emplace( labelName, basicBlock );
}

void Recompiler::CreateBranch( llvm::BasicBlock* basicBlock )
{
	m_IRBuilder.CreateBr( basicBlock );
}

void Recompiler::SetInsertPoint( llvm::BasicBlock* basicBlock )
{
	m_IRBuilder.SetInsertPoint( basicBlock );
}

llvm::Value* Recompiler::CreateLoadA( void )
{
	return m_IRBuilder.CreateLoad( &m_registerA, "" );
}

llvm::Value* Recompiler::CreateLoadX( void )
{
	return m_IRBuilder.CreateLoad( &m_registerX, "" );
}

llvm::Value* Recompiler::CreateLoadY( void )
{
	return m_IRBuilder.CreateLoad( &m_registerY, "" );
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

void Recompiler::PerformCmp( llvm::Value* lValue, llvm::Value* rValue )
{
	llvm::Value* diff = m_IRBuilder.CreateSub( lValue, rValue, "" );
	TestAndSetZero( diff );
	TestAndSetNegative( diff );
	TestAndSetCarrySubtraction( lValue, rValue );
}

void Recompiler::TestAndSetCarrySubtraction( llvm::Value* lValue, llvm::Value* rValue )
{
	llvm::Value* isCarry = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_UGE, lValue, rValue, "" );
	m_IRBuilder.CreateStore( isCarry, &m_registerStatusCarry );
}

void Recompiler::PerformLdy( llvm::Value* value )
{
	m_IRBuilder.CreateStore( value, &m_registerX );
	TestAndSetZero( value );
	TestAndSetNegative( value );
}

void Recompiler::PerformLdx( llvm::Value* value )
{
	m_IRBuilder.CreateStore( value, &m_registerX );
	TestAndSetZero( value );
	TestAndSetNegative( value );
}

void Recompiler::PerformLda( llvm::Value* value )
{
	m_IRBuilder.CreateStore( value, &m_registerA );
	TestAndSetZero( value );
	TestAndSetNegative( value );
}

void Recompiler::PerformOra( llvm::Value* value )
{
	llvm::LoadInst* loadA = m_IRBuilder.CreateLoad( &m_registerA, "" );
	llvm::Value* newA = m_IRBuilder.CreateOr( loadA, value, "" );
	m_IRBuilder.CreateStore( newA, &m_registerA );
	TestAndSetZero( newA );
	TestAndSetNegative( newA );
}

void Recompiler::TestAndSetZero( llvm::Value* value )
{
	llvm::Value* zeroConst = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, 0, false ) );
	llvm::Value* isZero = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_EQ, value, zeroConst, "" );
	m_IRBuilder.CreateStore( isZero, &m_registerStatusZero );
}

void Recompiler::TestAndSetNegative( llvm::Value* value )
{
	// TODO implement me.
}

void Recompiler::LoadAST( const char* filename )
{
	std::ifstream ifs( filename );
	if ( ifs.is_open() )
	{
		nlohmann::json j = nlohmann::json::parse( ifs );

		std::vector<nlohmann::json> ast;
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
					m_Program.emplace_back( Instruction{ node[ "Instruction" ][ "offset" ], node[ "Instruction" ][ "opcode" ], node[ "Instruction" ][ "operand" ] } );
				}
				else
				{
					m_Program.emplace_back( Instruction{ node[ "Instruction" ][ "offset" ], node[ "Instruction" ][ "opcode" ] } );
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

Recompiler::Instruction::Instruction( const uint32_t offset, const uint8_t opcode, const uint32_t operand )
	: m_Offset( offset )
	, m_Opcode( opcode )
	, m_Operand( operand )
	, m_HasOperand( true )
{
}

Recompiler::Instruction::Instruction( const uint32_t offset, const uint8_t opcode )
	: m_Offset( offset )
	, m_Opcode( opcode )
	, m_Operand( 0 )
	, m_HasOperand( false )
{
}

Recompiler::Instruction::~Instruction()
{

}
