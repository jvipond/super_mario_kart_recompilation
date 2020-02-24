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
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/CFG.h"
#include "llvm/Target/TargetMachine.h"

Recompiler::Recompiler()
: m_IRBuilder( m_LLVMContext )
, m_RecompilationModule( "recompilation", m_LLVMContext )
, m_RomResetAddr( 0 )
, m_StartFunction( nullptr )
, m_registerA( m_RecompilationModule, llvm::Type::getInt16Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "A" )
, m_registerDB( m_RecompilationModule, llvm::Type::getInt8Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "DB" )
, m_registerDP( m_RecompilationModule, llvm::Type::getInt16Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "DP" )
, m_registerPB( m_RecompilationModule, llvm::Type::getInt8Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "PB" )
, m_registerPC( m_RecompilationModule, llvm::Type::getInt16Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "PC" )
, m_registerSP( m_RecompilationModule, llvm::Type::getInt16Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "SP" )
, m_registerX( m_RecompilationModule, llvm::Type::getInt16Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "X" )
, m_registerY( m_RecompilationModule, llvm::Type::getInt16Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "Y" )
, m_registerP( m_RecompilationModule, llvm::Type::getInt8Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "P" )
, m_CarryFlag( m_RecompilationModule, llvm::Type::getInt1Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "CF" )
, m_ZeroFlag( m_RecompilationModule, llvm::Type::getInt1Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "ZF" )
, m_InterruptFlag( m_RecompilationModule, llvm::Type::getInt1Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "IF" )
, m_DecimalFlag( m_RecompilationModule, llvm::Type::getInt1Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "DF" )
, m_IndexRegisterFlag( m_RecompilationModule, llvm::Type::getInt1Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "XF" )
, m_AccumulatorFlag( m_RecompilationModule, llvm::Type::getInt1Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "MF" )
, m_OverflowFlag( m_RecompilationModule, llvm::Type::getInt1Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "VF" )
, m_NegativeFlag( m_RecompilationModule, llvm::Type::getInt1Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "NF" )
, m_EmulationFlag( m_RecompilationModule, llvm::Type::getInt1Ty( m_LLVMContext ), false, llvm::GlobalValue::ExternalLinkage, 0, "EF" )
, m_CurrentBasicBlock( nullptr )
, m_CycleFunction( nullptr )
, m_PanicFunction( nullptr )
, m_UpdateInstructionOutput( nullptr )
, m_Load8Function( nullptr )
, m_Store8Function( nullptr )
, m_DoPPUFrameFunction( nullptr )
, m_ADC8Function( nullptr )
, m_ADC16Function( nullptr )
, m_SBC8Function( nullptr )
, m_SBC16Function( nullptr )
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

	m_CarryFlag.removeFromParent();
	m_ZeroFlag.removeFromParent();
	m_InterruptFlag.removeFromParent();
	m_DecimalFlag.removeFromParent();
	m_IndexRegisterFlag.removeFromParent();
	m_AccumulatorFlag.removeFromParent();
	m_OverflowFlag.removeFromParent();
	m_NegativeFlag.removeFromParent();
	m_EmulationFlag.removeFromParent();
}

auto Recompiler::ConvertTo8( llvm::Value* value16 )
{
	auto low8 = m_IRBuilder.CreateTrunc( value16, llvm::Type::getInt8Ty( m_LLVMContext ) );
	auto high8 = m_IRBuilder.CreateTrunc( m_IRBuilder.CreateLShr( value16, 8 ), llvm::Type::getInt8Ty( m_LLVMContext ) );
	return std::make_tuple( low8, high8 );
}

void Recompiler::SetupIrqFunction()
{
	auto irqFunction = m_Functions[ m_RomIrqFuncName ];

	auto oldEntryBlock = &irqFunction->getEntryBlock();
	auto newEntryBlock = llvm::BasicBlock::Create( m_LLVMContext, m_RomIrqFuncName + "_" + "entryBlock", irqFunction );
	newEntryBlock->moveBefore( oldEntryBlock );
	SelectBlock( newEntryBlock );

	Push( m_IRBuilder.CreateLoad( &m_registerPB ) );
	auto pc16 = m_IRBuilder.CreateLoad( &m_registerPC );
	auto[ pclow8, pcHigh8 ] = ConvertTo8( pc16 );
	Push( pcHigh8 );
	Push( pclow8 );
	Push( GetProcessorStatusRegisterValueFromFlags() );

	m_IRBuilder.CreateBr( oldEntryBlock );

	m_CurrentBasicBlock = nullptr;
}

void Recompiler::SetupNmiCall()
{
	auto nmiFunction = m_Functions[ m_RomNmiFuncName ];
	auto& oldEntryBlock = nmiFunction->getEntryBlock();
	auto newEntryBlock = llvm::BasicBlock::Create( m_LLVMContext, "NMI_EntryPoint", nmiFunction );
	newEntryBlock->moveBefore( &oldEntryBlock );
	SelectBlock( newEntryBlock );

	Push( m_IRBuilder.CreateLoad( &m_registerPB ) );
	auto pc16 = m_IRBuilder.CreateLoad( &m_registerPC );
	auto [pclow8, pcHigh8] = ConvertTo8( pc16 );
	Push( pcHigh8 );
	Push( pclow8 );
	Push( GetProcessorStatusRegisterValueFromFlags() );

	m_IRBuilder.CreateBr( &oldEntryBlock );

	const auto& functionInfo = m_LabelsToFunctions.find( WAIT_FOR_VBLANK_LOOP_LABEL_OFFSET );
	if ( functionInfo != m_LabelsToFunctions.end() )
	{
		for ( const auto& functionEntry : functionInfo->second )
		{
			const auto basicBlockName = functionEntry.first + "_" + WAIT_FOR_VBLANK_LABEL_NAME;
			auto search = m_LabelNamesToBasicBlocks.find( basicBlockName );
			assert( search != m_LabelNamesToBasicBlocks.end() );

			auto basicBlock = search->second;
			auto firstInstruction = basicBlock->getFirstNonPHI();
			if ( firstInstruction )
			{
				auto callPPUFrameFunctionInst = m_IRBuilder.CreateCall( m_DoPPUFrameFunction );
				callPPUFrameFunctionInst->moveBefore( firstInstruction );
				m_IRBuilder.CreateCall( nmiFunction )->moveAfter( callPPUFrameFunctionInst );
			}
		}
	}
}

void Recompiler::FixReturnAddressManipulationFunctions()
{
	auto& functionList = m_RecompilationModule.getFunctionList();
	for ( auto& function : functionList )
	{
		for ( auto& basicBlock : function )
		{
			for ( auto& instruction : basicBlock )
			{
				if ( llvm::CallInst* callInst = llvm::dyn_cast<llvm::CallInst>( &instruction ) )
				{
					if ( llvm::Function* calledFunction = callInst->getCalledFunction() )
					{
						for ( auto& [functionName, _] : m_returnAddressManipulationFunctions )
						{
							if ( calledFunction->getName().startswith( functionName ) )
							{
								m_IRBuilder.SetInsertPoint( callInst->getNextNode() );

								auto needToReturn = m_IRBuilder.CreateICmp( llvm::CmpInst::ICMP_EQ, callInst, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 1, static_cast<uint64_t>( 1 ), false ) ) );
								auto then = llvm::SplitBlockAndInsertIfThen( needToReturn, callInst->getNextNode()->getNextNode(), false );
								m_IRBuilder.SetInsertPoint( then );
								m_IRBuilder.CreateRetVoid();
								then->eraseFromParent();
							}
						}
					}
				}
			}
		}
	}

	for ( auto&[ functionName, _ ] : m_returnAddressManipulationFunctions )
	{
		auto findFunction = m_Functions.find( functionName );
		if ( findFunction != m_Functions.end() )
		{
			auto function = findFunction->second;
			if ( function )
			{
				auto firstInstruction = function->getEntryBlock().getFirstNonPHI();
				if ( firstInstruction )
				{
					m_IRBuilder.SetInsertPoint( firstInstruction );
					auto returnValue = m_IRBuilder.CreateAlloca( llvm::Type::getInt1Ty( m_LLVMContext ), nullptr, "returnValue" );
					m_IRBuilder.CreateStore( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 1, static_cast<uint64_t>( 0 ), false ) ), returnValue );
					std::vector<llvm::Instruction*> oldReturnInstructions;
					for ( llvm::inst_iterator instruction = llvm::inst_begin( function ), end = llvm::inst_end( function ); instruction != end; ++instruction )
					{
						if ( llvm::isa<llvm::ReturnInst>( *instruction ) )
						{
							m_IRBuilder.SetInsertPoint( &( *instruction ) );
							m_IRBuilder.CreateRet( m_IRBuilder.CreateLoad( returnValue ) );
							oldReturnInstructions.push_back( &( *instruction ) );
						}
					}

					for ( auto oldReturnInstruction : oldReturnInstructions )
					{
						oldReturnInstruction->eraseFromParent();
					}

					auto findBasicBlock = m_returnAddressManipulationFunctionsBlocks.find( functionName );
					if ( findBasicBlock != m_returnAddressManipulationFunctionsBlocks.end() )
					{
						auto basicBlock = findBasicBlock->second;
						if ( basicBlock )
						{
							auto firstInstruction = basicBlock->getFirstNonPHI();
							if ( firstInstruction )
							{
								if ( llvm::isa<llvm::AllocaInst>( *firstInstruction ) )
								{
									llvm::AllocaInst* allocInst = llvm::dyn_cast<llvm::AllocaInst>( firstInstruction );
									if ( allocInst && allocInst->getName().compare( "returnValue" ) == 0 )
									{
										auto oldAssignmentInstruction = allocInst->getNextNode();
										m_IRBuilder.SetInsertPoint( oldAssignmentInstruction );
										m_IRBuilder.CreateStore( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 1, static_cast<uint64_t>( 1 ), false ) ), returnValue );
										oldAssignmentInstruction->eraseFromParent();
									}
								}
								else
								{
									m_IRBuilder.SetInsertPoint( firstInstruction );
									m_IRBuilder.CreateStore( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 1, static_cast<uint64_t>( 1 ), false ) ), returnValue );
								}
							}
						}
					}

					/*static const std::set< std::string > blocksToPatch = { "CODE_80D444", "CODE_80F109", "CODE_80F118", "CODE_81974E", "CODE_84DA38", "CODE_819A99" };
					for ( auto& blockToPatch : blocksToPatch )
					{
						const auto basicBlockName = functionName + "_" + blockToPatch;
						auto search = m_LabelNamesToBasicBlocks.find( basicBlockName );
						if ( search != m_LabelNamesToBasicBlocks.end() )
						{
							auto basicBlock = search->second;
							if ( basicBlock )
							{
								auto firstInstruction = basicBlock->getFirstNonPHI();
								if ( firstInstruction )
								{
									m_IRBuilder.SetInsertPoint( firstInstruction );
									m_IRBuilder.CreateStore( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 1, static_cast<uint64_t>( 1 ), false ) ), returnValue );
								}
							}
						}
					}*/
				}
			}
		}
	}
}

void Recompiler::EnforceFunctionEntryBlocksConstraints()
{
	for ( const auto& [functionName, function] : m_Functions )
	{
		auto& oldEntryBlock = function->getEntryBlock();
		if ( oldEntryBlock.hasNPredecessorsOrMore( 1 ) )
		{
			auto newEntryBlock = llvm::BasicBlock::Create( m_LLVMContext, functionName + "_" + "entryBlock", function );
			newEntryBlock->moveBefore( &oldEntryBlock );
			m_IRBuilder.SetInsertPoint( newEntryBlock );
			m_IRBuilder.CreateBr( &oldEntryBlock );
		}
	}
}

void Recompiler::CreateFunctions()
{
	auto voidReturnFunctionType = llvm::FunctionType::get( llvm::Type::getVoidTy( m_LLVMContext ), false );
	auto boolReturnFunctionType = llvm::FunctionType::get( llvm::Type::getInt1Ty( m_LLVMContext ), false );
	for ( const auto& functionName : m_FunctionNames )
	{
		llvm::FunctionType* functionReturnType = m_returnAddressManipulationFunctions.find( functionName ) == m_returnAddressManipulationFunctions.end() ? voidReturnFunctionType : boolReturnFunctionType;
		auto function = llvm::Function::Create( functionReturnType, llvm::Function::ExternalLinkage, functionName, m_RecompilationModule );
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
					auto functionSearch = m_returnAddressManipulationFunctions.find( functionEntry.first );
					auto search = m_LabelNamesToBasicBlocks.find( basicBlockName );
					assert( search != m_LabelNamesToBasicBlocks.end() );

					auto basicBlock = search->second;

					m_CurrentBasicBlock = basicBlock;
					m_IRBuilder.SetInsertPoint( basicBlock );

					auto codeGenIndex = nodeIndex + 1;
					auto hasAnyInstructions = false;
					while ( std::holds_alternative<Instruction>( m_Program[ codeGenIndex ] ) && codeGenIndex < numProgramNodes )
					{
						assert( std::holds_alternative<Instruction>( m_Program[ codeGenIndex ] ) );
						const auto& instruction = std::get<Instruction>( m_Program[ codeGenIndex ] );

						if ( functionSearch != m_returnAddressManipulationFunctions.end() && instruction.GetPC() == functionSearch->second )
						{
							m_returnAddressManipulationFunctionsBlocks.emplace( functionSearch->first, basicBlock );
						}
						GenerateCodeForInstruction( instruction, functionEntry.first );
						
						hasAnyInstructions = true;
						codeGenIndex++;
					}
					
					if ( !hasAnyInstructions )
					{
						m_IRBuilder.CreateCall( m_PanicFunction );
						m_IRBuilder.CreateRetVoid();
						m_CurrentBasicBlock = nullptr;
					}
					else if ( m_CurrentBasicBlock != nullptr && codeGenIndex < numProgramNodes )
					{
						const auto& nextLabel = std::get<Label>( m_Program[ codeGenIndex ] );
						const auto nextBasicBlockName = functionEntry.first + "_" + nextLabel.GetName();
						auto searchNextBasicBlockName = m_LabelNamesToBasicBlocks.find( nextBasicBlockName );
						
						if ( searchNextBasicBlockName != m_LabelNamesToBasicBlocks.end() )
						{	
							auto nextBasicBlock = searchNextBasicBlockName->second;
							m_IRBuilder.CreateBr( nextBasicBlock );
						}
						else
						{
							m_IRBuilder.CreateCall( m_PanicFunction );
							m_IRBuilder.CreateRetVoid();
							m_CurrentBasicBlock = nullptr;
						}
					}		
				}
			}
		}
	}
}

void Recompiler::Recompile()
{
	llvm::InitializeNativeTarget();
	
#ifdef __EMSCRIPTEN__
	m_RecompilationModule.setDataLayout( "e-m:e-p:32:32-i64:64-n32:64-S128" );
	m_RecompilationModule.setTargetTriple( "wasm32" );
#endif // __EMSCRIPTEN__

	// Add cycle function that will called every time an instruction is executed:
	m_CycleFunction = llvm::Function::Create( llvm::FunctionType::get( llvm::Type::getVoidTy( m_LLVMContext ), { llvm::Type::getInt32Ty( m_LLVMContext ), llvm::Type::getInt32Ty( m_LLVMContext )}, false ), llvm::Function::ExternalLinkage, "romCycle", m_RecompilationModule );
	m_UpdateInstructionOutput = llvm::Function::Create( llvm::FunctionType::get( llvm::Type::getVoidTy( m_LLVMContext ), { llvm::Type::getInt32Ty( m_LLVMContext ), llvm::Type::getInt8PtrTy( m_LLVMContext ) }, false ), llvm::Function::ExternalLinkage, "updateInstructionOutput", m_RecompilationModule );
	m_PanicFunction = llvm::Function::Create( llvm::FunctionType::get( llvm::Type::getVoidTy( m_LLVMContext ), false ), llvm::Function::ExternalLinkage, "panic", m_RecompilationModule );

	m_Load8Function = llvm::Function::Create( llvm::FunctionType::get( llvm::Type::getInt8Ty( m_LLVMContext ), llvm::Type::getInt32Ty( m_LLVMContext ), false ), llvm::Function::ExternalLinkage, "read8", m_RecompilationModule );
	m_Store8Function = llvm::Function::Create( llvm::FunctionType::get( llvm::Type::getVoidTy( m_LLVMContext ), { llvm::Type::getInt32Ty( m_LLVMContext ), llvm::Type::getInt8Ty( m_LLVMContext ) }, false ), llvm::Function::ExternalLinkage, "write8", m_RecompilationModule );
	
	m_DoPPUFrameFunction = llvm::Function::Create( llvm::FunctionType::get( llvm::Type::getVoidTy( m_LLVMContext ), false ), llvm::Function::ExternalLinkage, "doPPUFrame", m_RecompilationModule );

	m_ADC8Function = llvm::Function::Create( llvm::FunctionType::get( llvm::Type::getInt8Ty( m_LLVMContext ), llvm::Type::getInt8Ty( m_LLVMContext ), false ), llvm::Function::ExternalLinkage, "ADC8", m_RecompilationModule );
	m_ADC16Function = llvm::Function::Create( llvm::FunctionType::get( llvm::Type::getInt16Ty( m_LLVMContext ), llvm::Type::getInt16Ty( m_LLVMContext ), false ), llvm::Function::ExternalLinkage, "ADC16", m_RecompilationModule );
	m_SBC8Function = llvm::Function::Create( llvm::FunctionType::get( llvm::Type::getInt8Ty( m_LLVMContext ), llvm::Type::getInt8Ty( m_LLVMContext ), false ), llvm::Function::ExternalLinkage, "SBC8", m_RecompilationModule );
	m_SBC16Function = llvm::Function::Create( llvm::FunctionType::get( llvm::Type::getInt16Ty( m_LLVMContext ), llvm::Type::getInt16Ty( m_LLVMContext ), false ), llvm::Function::ExternalLinkage, "SBC16", m_RecompilationModule );

	auto mainFunctionType = llvm::FunctionType::get( llvm::Type::getVoidTy( m_LLVMContext ), false );
	m_StartFunction = llvm::Function::Create( mainFunctionType, llvm::Function::ExternalLinkage, "start", m_RecompilationModule );

	auto entry = llvm::BasicBlock::Create( m_LLVMContext, "EntryBlock", m_StartFunction );
	auto panicBlock = llvm::BasicBlock::Create( m_LLVMContext, "PanicBlock", m_StartFunction );
	m_IRBuilder.SetInsertPoint( panicBlock );
	m_IRBuilder.CreateCall( m_PanicFunction );
	m_IRBuilder.CreateRetVoid();
	m_IRBuilder.SetInsertPoint( entry );

	AddInstructionStringGlobalVariables();
	CreateFunctions();
	InitialiseBasicBlocksFromLabelNames();
	GenerateCode();
	EnforceFunctionEntryBlocksConstraints();
	SetupNmiCall();
	SetupIrqFunction();
	FixReturnAddressManipulationFunctions();

	SelectBlock( entry );
	m_IRBuilder.CreateStore( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 16, m_RomResetAddr, true ) ), &m_registerPC );
	
	auto resetFunction = m_Functions[ m_RomResetFuncName ];
	m_IRBuilder.CreateCall( resetFunction );
	m_IRBuilder.CreateRetVoid();
	llvm::verifyModule( m_RecompilationModule, &llvm::errs() );

#ifdef __EMSCRIPTEN__
	llvm::raw_fd_ostream binaryOutput( "smk.bc", EC );
	llvm::WriteBitcodeToFile( m_RecompilationModule, binaryOutput );
	binaryOutput.flush();
#else
	std::error_code EC;
	llvm::raw_fd_ostream outputHumanReadable( "smk.ll", EC );
	m_RecompilationModule.print( outputHumanReadable, nullptr );
#endif // __EMSCRIPTEN__
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

void Recompiler::SelectBlock( llvm::BasicBlock* basicBlock )
{
	m_IRBuilder.SetInsertPoint( basicBlock );
	m_CurrentBasicBlock = basicBlock;
}

void Recompiler::SetInsertPoint( llvm::BasicBlock* basicBlock )
{
	m_IRBuilder.SetInsertPoint( basicBlock );
}

void Recompiler::PerformRomCycle( llvm::Value* value )
{
	std::vector<llvm::Value*> params = { value, llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, 1, false ) ) };
	m_IRBuilder.CreateCall( m_CycleFunction, params );
}

void Recompiler::PerformUpdateInstructionOutput( const uint32_t offset, const uint32_t pc, const std::string& instructionString )
{
	auto pb32 = GetConstant( (pc & 0xff0000) >> 16, 32, false );
	auto pb8 = m_IRBuilder.CreateTrunc( pb32, llvm::Type::getInt8Ty( m_LLVMContext ) );
	m_IRBuilder.CreateStore( pb8, &m_registerPB );

	auto pc32 = GetConstant( pc & 0xffff, 32, false );
	auto pc16 = m_IRBuilder.CreateTrunc( pc32, llvm::Type::getInt16Ty( m_LLVMContext ) );
	m_IRBuilder.CreateStore( pc16, &m_registerPC );

	auto s = m_OffsetsToInstructionStringGlobalVariable[ offset ];
	std::vector<llvm::Value*> params = { GetConstant( pc, 32, false ), m_IRBuilder.CreateConstGEP2_32( s->getValueType(), s, 0, 0, "" ) };
	m_IRBuilder.CreateCall( m_UpdateInstructionOutput, params, "" ); 
}

llvm::Value* Recompiler::Read8( llvm::Value* address )
{
	return m_IRBuilder.CreateCall( m_Load8Function, address );
}

void Recompiler::Write8( llvm::Value* address, llvm::Value* value )
{
	m_IRBuilder.CreateCall( m_Store8Function, { address, value } );
}

llvm::Value* Recompiler::LoadRegister32( llvm::Value* value )
{
	auto r = m_IRBuilder.CreateLoad( value );
	return m_IRBuilder.CreateZExt( r, llvm::Type::getInt32Ty( m_LLVMContext ) );
}

llvm::Value* Recompiler::CreateDirectAddress( llvm::Value* address )
{
	auto D = LoadRegister32( &m_registerDP );
	auto finalAddress = m_IRBuilder.CreateAdd( D, address );
	return m_IRBuilder.CreateAnd( finalAddress, 0xffff );
}

llvm::Value* Recompiler::CreateDirectEmulationAddress( llvm::Value* address )
{
	auto DP16 = m_IRBuilder.CreateLoad( &m_registerDP );
	auto DP32 = m_IRBuilder.CreateZExt( DP16, llvm::Type::getInt32Ty( m_LLVMContext ) );
	return m_IRBuilder.CreateAnd( m_IRBuilder.CreateOr( address, DP32 ), 0xff );
}

auto Recompiler::GetLowHighPtrFromPtr16( llvm::Value* ptr16 )
{
	auto low8Ptr = m_IRBuilder.CreateBitCast( ptr16, llvm::Type::getInt8PtrTy( m_LLVMContext ) );
	auto high8Ptr = m_IRBuilder.CreateInBoundsGEP( llvm::Type::getInt8Ty( m_LLVMContext ), low8Ptr, GetConstant( 1, 32, false ) );
	return std::make_tuple( low8Ptr, high8Ptr );
}

auto Recompiler::CreateCondTestThenElseBlock( llvm::Value* cond )
{
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
	return std::make_tuple( thenBlock, elseBlock, endBlock );
}

auto Recompiler::CreateCondTestThenBlock( llvm::Value* cond )
{
	auto thenBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto endBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	if ( m_CurrentBasicBlock )
	{
		thenBlock->moveAfter( m_CurrentBasicBlock );
		endBlock->moveAfter( thenBlock );
	}

	m_IRBuilder.CreateCondBr( cond, thenBlock, endBlock );
	return std::make_tuple( thenBlock, endBlock );
}

llvm::Value* Recompiler::ReadDirect( llvm::Value* address )
{
	auto[ dpLow8Ptr, dpHigh8Ptr ] = Recompiler::GetLowHighPtrFromPtr16( &m_registerDP );
	auto dpLow8 = m_IRBuilder.CreateLoad( dpLow8Ptr );

	auto dpLow8EqualZero = m_IRBuilder.CreateICmpEQ( dpLow8, GetConstant( 0, 8, false ) );
	auto emulationFlagSet = m_IRBuilder.CreateLoad( &m_EmulationFlag );
	auto cond = m_IRBuilder.CreateAnd( emulationFlagSet, dpLow8EqualZero );

	auto[ thenBlock, elseBlock, endBlock ] = CreateCondTestThenElseBlock( cond );

	SelectBlock( thenBlock );
	auto directEmulationReadAddress = CreateDirectEmulationAddress( address );
	auto read8Emulation = Read8( directEmulationReadAddress );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto directReadAddress = CreateDirectAddress( address );
	auto read8 = Read8( directReadAddress );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
	auto phi = m_IRBuilder.CreatePHI( llvm::Type::getInt8Ty( m_LLVMContext ), 2 );
	phi->addIncoming( read8Emulation, thenBlock );
	phi->addIncoming( read8, elseBlock );
	return phi;
}

llvm::Value* Recompiler::ReadDirectNative( llvm::Value* address )
{
	auto directReadAddress = CreateDirectAddress( address );
	return Read8( directReadAddress );
}

void Recompiler::WriteDirect( llvm::Value* address, llvm::Value* value )
{
	auto[ dpLow8Ptr, dpHigh8Ptr ] = Recompiler::GetLowHighPtrFromPtr16( &m_registerDP );
	auto dpLow8 = m_IRBuilder.CreateLoad( dpLow8Ptr );

	auto dpLow8EqualZero = m_IRBuilder.CreateICmpEQ( dpLow8, GetConstant( 0, 8, false ) );
	auto emulationFlagSet = m_IRBuilder.CreateLoad( &m_EmulationFlag );
	auto cond = m_IRBuilder.CreateAnd( emulationFlagSet, dpLow8EqualZero );

	auto [thenBlock, elseBlock, endBlock] = CreateCondTestThenElseBlock( cond );

	SelectBlock( thenBlock );
	auto directEmulationWriteAddress = CreateDirectEmulationAddress( address );
	Write8( directEmulationWriteAddress, value );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	auto directWriteAddress = CreateDirectAddress( address );
	Write8( directWriteAddress, value );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

llvm::Value* Recompiler::CreateBankAddress( llvm::Value* address )
{
	auto B = LoadRegister32( &m_registerDB );
	auto shiftedBank = m_IRBuilder.CreateShl( B, 16 );
	auto finalAddress = m_IRBuilder.CreateAdd( shiftedBank, address );
	return m_IRBuilder.CreateAnd( finalAddress, 0xffffff );
}

llvm::Value* Recompiler::ReadBank( llvm::Value* address )
{
	auto bankReadAddress = CreateBankAddress( address );
	return Read8( bankReadAddress );
}

void Recompiler::WriteBank( llvm::Value* address, llvm::Value* value )
{
	auto bankWriteAddress = CreateBankAddress( address );
	return Write8( bankWriteAddress, value );
}

llvm::Value* Recompiler::ReadLong( llvm::Value* address )
{
	return Read8( m_IRBuilder.CreateAnd( address, 0xffffff ) );
}

void Recompiler::WriteLong( llvm::Value* address, llvm::Value* value )
{
	Write8( m_IRBuilder.CreateAnd( address, 0xffffff ), value );
}

llvm::Value* Recompiler::CreateStackAddress( llvm::Value* address )
{
	auto S = LoadRegister32( &m_registerSP );
	auto finalAddress = m_IRBuilder.CreateAdd( S, address );
	return m_IRBuilder.CreateAnd( finalAddress, 0xffff );
}

llvm::Value* Recompiler::ReadStack( llvm::Value* address )
{
	auto stackReadAddress = CreateStackAddress( address );
	return Read8( stackReadAddress );
}

void Recompiler::WriteStack( llvm::Value* address, llvm::Value* value )
{
	auto stackWriteAddress = CreateStackAddress( address );
	Write8( stackWriteAddress, value );
}

llvm::Value* Recompiler::Pull()
{
	auto EF = m_IRBuilder.CreateLoad( &m_EmulationFlag );
	auto EFCond = m_IRBuilder.CreateICmpEQ( EF, GetConstant( 1, 1, false ) );
	auto[ thenBlockEmulationFlagTest, elseBlockEmulationFlagTest, endBlockEmulationFlagTest ] = CreateCondTestThenElseBlock( EFCond );

	SelectBlock( thenBlockEmulationFlagTest );
	auto[ spLow8Ptr, spHigh8Ptr ] = GetLowHighPtrFromPtr16( &m_registerSP );
	auto spLow8 = m_IRBuilder.CreateLoad( spLow8Ptr );
	auto spLow8AddOne = m_IRBuilder.CreateAdd( spLow8, GetConstant( 1, 8, false ) );
	m_IRBuilder.CreateStore( spLow8AddOne, spLow8Ptr );
	m_IRBuilder.CreateBr( endBlockEmulationFlagTest );

	SelectBlock( elseBlockEmulationFlagTest );
	auto SP16Value = m_IRBuilder.CreateLoad( &m_registerSP );
	auto SP16AddOne = m_IRBuilder.CreateAdd( SP16Value, GetConstant( 1, 16, false ) );
	m_IRBuilder.CreateStore( SP16AddOne, &m_registerSP );
	m_IRBuilder.CreateBr( endBlockEmulationFlagTest );

	SelectBlock( endBlockEmulationFlagTest );

	return Read8( m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerSP ), llvm::Type::getInt32Ty( m_LLVMContext ) ) );
}

llvm::Value* Recompiler::PullNative()
{
	auto SP16 = m_IRBuilder.CreateLoad( &m_registerSP );
	auto SP16PlusOne = m_IRBuilder.CreateAdd( SP16, GetConstant( 1, 16, false ) );
	auto SP32PlusOne = m_IRBuilder.CreateZExt( SP16PlusOne, llvm::Type::getInt32Ty( m_LLVMContext ) );
	m_IRBuilder.CreateStore( SP16PlusOne, &m_registerSP );
	return Read8( SP32PlusOne );
}

void Recompiler::Push( llvm::Value* value8 )
{
	auto SP16 = m_IRBuilder.CreateLoad( &m_registerSP );
	auto SP32 = m_IRBuilder.CreateZExt( SP16, llvm::Type::getInt32Ty( m_LLVMContext ) );

	Write8( SP32, value8 );

	auto EF = m_IRBuilder.CreateLoad( &m_EmulationFlag );
	auto EFCond = m_IRBuilder.CreateICmpEQ( EF, GetConstant( 1, 1, false ) );
	auto[ thenBlockEmulationFlagTest, elseBlockEmulationFlagTest, endBlockEmulationFlagTest ] = CreateCondTestThenElseBlock( EFCond );

	SelectBlock( thenBlockEmulationFlagTest );
	auto[ spLow8Ptr, spHigh8Ptr ] = GetLowHighPtrFromPtr16( &m_registerSP );
	auto spLow8 = m_IRBuilder.CreateLoad( spLow8Ptr );
	auto spLow8MinusOne = m_IRBuilder.CreateSub( spLow8, GetConstant( 1, 8, false ) );
	m_IRBuilder.CreateStore( spLow8MinusOne, spLow8Ptr );
	m_IRBuilder.CreateBr( endBlockEmulationFlagTest );

	SelectBlock( elseBlockEmulationFlagTest );
	auto SP16MinusOne = m_IRBuilder.CreateSub( SP16, GetConstant( 1, 16, false ) );
	m_IRBuilder.CreateStore( SP16MinusOne, &m_registerSP );
	m_IRBuilder.CreateBr( endBlockEmulationFlagTest );

	SelectBlock( endBlockEmulationFlagTest );
}

void Recompiler::PushNative( llvm::Value* value8 )
{
	auto SP16 = m_IRBuilder.CreateLoad( &m_registerSP );
	auto SP32 = m_IRBuilder.CreateZExt( SP16, llvm::Type::getInt32Ty( m_LLVMContext ) );

	Write8( SP32, value8 );

	auto SP16MinusOne = m_IRBuilder.CreateSub( SP16, GetConstant( 1, 16, false ) );
	m_IRBuilder.CreateStore( SP16MinusOne, &m_registerSP );
}

void Recompiler::InstructionImmediateRead8( Operation op, llvm::Value* operand8 )
{
	( this->*op )( operand8 );
}

void Recompiler::InstructionImmediateRead16( Operation op, llvm::Value* operand16 )
{
	( this->*op )( operand16 );
}

void Recompiler::PerformSetFlagInstruction( llvm::Value* flag )
{
	m_IRBuilder.CreateStore( GetConstant( 1, 1, false ), flag );
}

void Recompiler::PerformClearFlagInstruction( llvm::Value* flag )
{
	m_IRBuilder.CreateStore( GetConstant( 0, 1, false ), flag );
}

void Recompiler::PerformExchangeCEInstruction()
{
	auto newEmulationFlagValue = m_IRBuilder.CreateLoad( &m_CarryFlag );
	auto newCarryFlagValue = m_IRBuilder.CreateLoad( &m_EmulationFlag );
	m_IRBuilder.CreateStore( newEmulationFlagValue, &m_EmulationFlag );
	m_IRBuilder.CreateStore( newCarryFlagValue, &m_CarryFlag );

	auto thenBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto endBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );

	if ( m_CurrentBasicBlock )
	{
		thenBlock->moveAfter( m_CurrentBasicBlock );
		endBlock->moveAfter( thenBlock );
	}

	m_IRBuilder.CreateCondBr( newEmulationFlagValue, thenBlock, endBlock );
	SelectBlock( thenBlock );
	PerformSetFlagInstruction( &m_IndexRegisterFlag );
	PerformSetFlagInstruction( &m_AccumulatorFlag );
	auto [xLow8Ptr, xHigh8Ptr]  = GetLowHighPtrFromPtr16( &m_registerX );
	m_IRBuilder.CreateStore( GetConstant( 0, 8, false ), xHigh8Ptr );

	auto [ yLow8Ptr, yHigh8Ptr ] = GetLowHighPtrFromPtr16( &m_registerY );
	m_IRBuilder.CreateStore( GetConstant( 0, 8, false ), yHigh8Ptr );

	auto [ spLow8Ptr, spHigh8Ptr ] = GetLowHighPtrFromPtr16( &m_registerSP );
	m_IRBuilder.CreateStore( GetConstant( 1, 8, false ), spHigh8Ptr );
	m_IRBuilder.CreateBr( endBlock );
	
	SelectBlock( endBlock );
}

void Recompiler::PerformExchangeBAInstruction()
{
	auto [ aLow8Ptr, aHigh8Ptr ] = GetLowHighPtrFromPtr16( &m_registerA );
	auto aLow8 = m_IRBuilder.CreateLoad( aLow8Ptr );
	auto aHigh8 = m_IRBuilder.CreateLoad( aHigh8Ptr );

	m_IRBuilder.CreateStore( aLow8, aHigh8Ptr );
	m_IRBuilder.CreateStore( aHigh8, aLow8Ptr );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( aHigh8, GetConstant( 0, 8, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits8( aHigh8, 0x80 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );
}

llvm::Value* Recompiler::LoadFlag8( llvm::Value* flagPtr )
{
	auto flagValue = m_IRBuilder.CreateLoad( flagPtr );
	return m_IRBuilder.CreateZExt( flagValue, llvm::Type::getInt8Ty( m_LLVMContext ) );
}

llvm::Value* Recompiler::OrAllValues( llvm::Value* v ) 
{
	return v;
}

template<typename... T>
auto Recompiler::OrAllValues( llvm::Value* s, T... ts ) 
{
	return m_IRBuilder.CreateOr( s, OrAllValues( ts ... ) );
}

llvm::Value* Recompiler::AddAllValues( llvm::Value* v )
{
	return v;
}

template<typename... T>
auto Recompiler::AddAllValues( llvm::Value* s, T... ts )
{
	return m_IRBuilder.CreateAdd( s, AddAllValues( ts ... ) );
}

llvm::Value* Recompiler::GetProcessorStatusRegisterValueFromFlags()
{
	auto c8 = m_IRBuilder.CreateShl( LoadFlag8( &m_CarryFlag ), 0ull );
	auto z8 = m_IRBuilder.CreateShl( LoadFlag8( &m_ZeroFlag ), 1ull );
	auto i8 = m_IRBuilder.CreateShl( LoadFlag8( &m_InterruptFlag ), 2ull );
	auto d8 = m_IRBuilder.CreateShl( LoadFlag8( &m_DecimalFlag ), 3ull );
	auto x8 = m_IRBuilder.CreateShl( LoadFlag8( &m_IndexRegisterFlag ), 4ull );
	auto m8 = m_IRBuilder.CreateShl( LoadFlag8( &m_AccumulatorFlag ), 5ull );
	auto v8 = m_IRBuilder.CreateShl( LoadFlag8( &m_OverflowFlag ), 6ull );
	auto n8 = m_IRBuilder.CreateShl( LoadFlag8( &m_NegativeFlag ), 7ull );

	return OrAllValues( c8, z8, i8, d8, x8, m8, v8, n8 );
}

void Recompiler::SetProcessorStatusFlagsFromValue( llvm::Value* status8 )
{
	auto carryFlagResult = TestBits8( status8, 0x01 );
	m_IRBuilder.CreateStore( carryFlagResult, &m_CarryFlag );

	auto zeroFlagResult = TestBits8( status8, 0x02 );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto interruptFlagResult = TestBits8( status8, 0x04 );
	m_IRBuilder.CreateStore( interruptFlagResult, &m_InterruptFlag );

	auto decimalFlagResult = TestBits8( status8, 0x08 );
	m_IRBuilder.CreateStore( decimalFlagResult, &m_DecimalFlag );

	auto indexFlagResult = TestBits8( status8, 0x10 );
	m_IRBuilder.CreateStore( indexFlagResult, &m_IndexRegisterFlag );

	auto accumulatorFlagResult = TestBits8( status8, 0x20 );
	m_IRBuilder.CreateStore( accumulatorFlagResult, &m_AccumulatorFlag );

	auto overflowFlagResult = TestBits8( status8, 0x40 );
	m_IRBuilder.CreateStore( overflowFlagResult, &m_OverflowFlag );

	auto negativeFlagResult = TestBits8( status8, 0x80 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );
}

void Recompiler::PerformProcessorStatusRegisterForcedConfiguration()
{
	auto EF = m_IRBuilder.CreateLoad( &m_EmulationFlag );
	auto EFCond = m_IRBuilder.CreateICmpEQ( EF, GetConstant( 1, 1, false ) );
	auto[ thenBlockEmulationFlagTest, endBlockEmulationFlagTest ] = CreateCondTestThenBlock( EFCond );

	SelectBlock( thenBlockEmulationFlagTest );
	PerformSetFlagInstruction( &m_IndexRegisterFlag );
	PerformSetFlagInstruction( &m_AccumulatorFlag );
	m_IRBuilder.CreateBr( endBlockEmulationFlagTest );

	SelectBlock( endBlockEmulationFlagTest );
	auto XF = m_IRBuilder.CreateLoad( &m_IndexRegisterFlag );
	auto XFCond = m_IRBuilder.CreateICmpEQ( XF, GetConstant( 1, 1, false ) );
	auto[ thenBlockIndexFlagTest, endBlockIndexFlagTest ] = CreateCondTestThenBlock( XFCond );

	SelectBlock( thenBlockIndexFlagTest );
	auto[ xlow8Ptr, xHigh8Ptr ] = GetLowHighPtrFromPtr16( &m_registerX );
	m_IRBuilder.CreateStore( GetConstant( 0, 8, false ), xHigh8Ptr );

	auto[ ylow8Ptr, yHigh8Ptr ] = GetLowHighPtrFromPtr16( &m_registerY );
	m_IRBuilder.CreateStore( GetConstant( 0, 8, false ), yHigh8Ptr );
	m_IRBuilder.CreateBr( endBlockIndexFlagTest );

	SelectBlock( endBlockIndexFlagTest );
}

void Recompiler::PerformResetPInstruction( llvm::Value* operand8 )
{
	auto P = GetProcessorStatusRegisterValueFromFlags();
	auto complementOperand8 = m_IRBuilder.CreateXor( operand8, GetConstant( 0xff, 8, false ) );
	auto result = m_IRBuilder.CreateAnd( P, complementOperand8 );
	SetProcessorStatusFlagsFromValue( result );

	PerformProcessorStatusRegisterForcedConfiguration();
}

void Recompiler::PerformSetPInstruction( llvm::Value* operand8 )
{
	auto P = GetProcessorStatusRegisterValueFromFlags();
	auto result = m_IRBuilder.CreateOr( P, operand8 );
	SetProcessorStatusFlagsFromValue( result );

	PerformProcessorStatusRegisterForcedConfiguration();
}

auto Recompiler::CreateRegisterFlagTestBlock( llvm::Value* flagPtr )
{
	auto flag = m_IRBuilder.CreateLoad( flagPtr );
	auto cond = m_IRBuilder.CreateICmpEQ( flag, GetConstant( 1, 1, false ) );
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
	return std::make_tuple( thenBlock, elseBlock, endBlock );
}

void Recompiler::PerformTransferInstruction( RegisterModeFlag modeFlag, llvm::Value* sourceRegisterPtr, llvm::Value* destinationRegisterPtr )
{
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );

	SelectBlock( flagSetBlock );
	InstructionTransfer8( sourceRegisterPtr, destinationRegisterPtr );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionTransfer16( sourceRegisterPtr, destinationRegisterPtr );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformTransfer16Instruction( llvm::Value* sourceRegisterPtr, llvm::Value* destinationRegisterPtr )
{
	InstructionTransfer16( sourceRegisterPtr, destinationRegisterPtr );
}

void Recompiler::PerformTransferCSInstruction()
{
	auto A16 = m_IRBuilder.CreateLoad( &m_registerA );
	m_IRBuilder.CreateStore( A16, &m_registerSP );

	PerformStackPointerEmulationFlagForcedConfiguration();
}

void Recompiler::PerformTransferSXInstruction( RegisterModeFlag modeFlag )
{
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );

	SelectBlock( flagSetBlock );
	InstructionTransferSX8();
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionTransferSX16();
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformTransferXSInstruction()
{
	auto EF = m_IRBuilder.CreateLoad( &m_EmulationFlag );
	auto EFCond = m_IRBuilder.CreateICmpEQ( EF, GetConstant( 1, 1, false ) );
	auto[ thenBlockEmulationFlagTest, elseBlockEmulationFlagTest, endBlockEmulationFlagTest ] = CreateCondTestThenElseBlock( EFCond );

	SelectBlock( thenBlockEmulationFlagTest );
	auto[ xLow8Ptr, xHigh8Ptr ] = GetLowHighPtrFromPtr16( &m_registerX );
	auto[ spLow8Ptr, spHigh8Ptr ] = GetLowHighPtrFromPtr16( &m_registerSP );

	auto xlow8Value = m_IRBuilder.CreateLoad( xLow8Ptr );
	m_IRBuilder.CreateStore( xlow8Value, spLow8Ptr );
	m_IRBuilder.CreateBr( endBlockEmulationFlagTest );

	SelectBlock( elseBlockEmulationFlagTest );
	auto X16Value = m_IRBuilder.CreateLoad( &m_registerX );
	m_IRBuilder.CreateStore( X16Value, &m_registerSP );
	m_IRBuilder.CreateBr( endBlockEmulationFlagTest );

	SelectBlock( endBlockEmulationFlagTest );
}

void Recompiler::PerformPushInstruction( RegisterModeFlag modeFlag, llvm::Value* value16 )
{
	auto[ low8, high8 ] = ConvertTo8( value16 );

	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );

	SelectBlock( flagSetBlock );
	InstructionPush8( low8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionPush16( low8, high8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformPush8Instruction( llvm::Value* value8 )
{
	InstructionPush8( value8 );
}

void Recompiler::PerformPushDInstruction()
{
	auto[ dLow8Ptr, dHigh8Ptr ] = GetLowHighPtrFromPtr16( &m_registerDP );
	auto dLow8Value = m_IRBuilder.CreateLoad( dLow8Ptr );
	auto dHigh8Value = m_IRBuilder.CreateLoad( dHigh8Ptr );
	PushNative( dHigh8Value );
	PushNative( dLow8Value );
	
	PerformStackPointerEmulationFlagForcedConfiguration();
}

void Recompiler::PerformStackPointerEmulationFlagForcedConfiguration()
{
	auto EF = m_IRBuilder.CreateLoad( &m_EmulationFlag );
	auto EFCond = m_IRBuilder.CreateICmpEQ( EF, GetConstant( 1, 1, false ) );
	auto[ thenBlockEmulationFlagTest, endBlockEmulationFlagTest ] = CreateCondTestThenBlock( EFCond );

	SelectBlock( thenBlockEmulationFlagTest );
	auto[ spLow8Ptr, spHigh8Ptr ] = GetLowHighPtrFromPtr16( &m_registerSP );
	m_IRBuilder.CreateStore( GetConstant( 1, 8, false ), spHigh8Ptr );
	m_IRBuilder.CreateBr( endBlockEmulationFlagTest );

	SelectBlock( endBlockEmulationFlagTest );
}

void Recompiler::PerformBranchInstruction( llvm::Value* cond, const std::string& labelName, const std::string& functionName )
{
	auto search = m_LabelNamesToBasicBlocks.find( functionName + "_" + labelName );
	if ( search != m_LabelNamesToBasicBlocks.end() )
	{
		auto [takeBranchBlock, endBlock] = CreateCondTestThenBlock( cond );
		SelectBlock( takeBranchBlock );
		m_IRBuilder.CreateBr( search->second );
		SelectBlock( endBlock );
	}
	else
	{
		m_IRBuilder.CreateCall( m_PanicFunction );
		m_IRBuilder.CreateRetVoid();
		m_CurrentBasicBlock = nullptr;
	}
}

void Recompiler::PerformJumpInstruction( const std::string& labelName, const std::string& functionName )
{
	auto search = m_LabelNamesToBasicBlocks.find( functionName + "_" + labelName );
	if ( search != m_LabelNamesToBasicBlocks.end() )
	{
		m_IRBuilder.CreateBr( search->second );
	}
	else
	{
		m_IRBuilder.CreateCall( m_PanicFunction );
		m_IRBuilder.CreateRetVoid();
	}

	m_CurrentBasicBlock = nullptr;
}

llvm::Value* Recompiler::GetPBPC32()
{
	auto pc16 = m_IRBuilder.CreateLoad( &m_registerPC );
	auto pc32 = m_IRBuilder.CreateZExt( pc16, llvm::Type::getInt32Ty( m_LLVMContext ) );
	
	auto pb8 = m_IRBuilder.CreateLoad( &m_registerPB );
	auto pb32 = m_IRBuilder.CreateZExt( pb8, llvm::Type::getInt32Ty( m_LLVMContext ) );

	auto shiftedPB32 = m_IRBuilder.CreateShl( pb32, 16 );
	
	return m_IRBuilder.CreateOr( shiftedPB32, pc32 );
}

void Recompiler::InsertJumpTable( llvm::Value* switchValue, const uint32_t instructionOffset, const std::string& functionName )
{
	auto findJumpTableEntries = m_JumpTables.find( instructionOffset );
	assert( findJumpTableEntries != m_JumpTables.end() );

	auto jumpTableEntries = findJumpTableEntries->second;
	const auto numSwitchCases = jumpTableEntries.size();
	auto endBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto panicBlock = llvm::BasicBlock::Create( m_LLVMContext, "PanicBlock", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );

	auto sw = m_IRBuilder.CreateSwitch( switchValue, endBlock, numSwitchCases );
	for ( const auto& jumpTableEntry : jumpTableEntries )
	{
		const auto& labelName = functionName + "_" + jumpTableEntry.second;
		auto findLabelResult = m_LabelNamesToBasicBlocks.find( labelName );
		if ( findLabelResult != m_LabelNamesToBasicBlocks.end() )
		{
			auto basicBlock = findLabelResult->second;
			if ( basicBlock )
			{
				auto caseBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
				SelectBlock( caseBlock );
				m_IRBuilder.CreateBr( basicBlock );
				auto caseValue = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( jumpTableEntry.first ), false ) );
				sw->addCase( caseValue, caseBlock );
			}
		}
	}

	m_IRBuilder.SetInsertPoint( panicBlock );
	m_IRBuilder.CreateCall( m_PanicFunction );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformJumpIndirectInstruction( const uint32_t instructionOffset, llvm::Value* operand16, const std::string& functionName )
{
	auto low8PC = Read8( m_IRBuilder.CreateZExt( operand16, llvm::Type::getInt32Ty( m_LLVMContext ) ) );
	auto high8PC = Read8( m_IRBuilder.CreateZExt( m_IRBuilder.CreateAdd( operand16, GetConstant( 1, 16, false ) ), llvm::Type::getInt32Ty( m_LLVMContext ) ) );

	auto [pclow8Ptr, pcHigh8Ptr] = GetLowHighPtrFromPtr16( &m_registerPC );
	m_IRBuilder.CreateStore( low8PC, pclow8Ptr );
	m_IRBuilder.CreateStore( high8PC, pcHigh8Ptr );

	auto jumpAddress = GetPBPC32();
	InsertJumpTable( jumpAddress, instructionOffset, functionName );
}

void Recompiler::PerformJumpIndexedIndirectInstruction( const uint32_t instructionOffset, llvm::Value* operand16, const std::string& functionName )
{
	auto PB8 = m_IRBuilder.CreateLoad( &m_registerPB );
	auto PB32 = m_IRBuilder.CreateZExt( PB8, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto shiftedPB32 = m_IRBuilder.CreateShl( PB32, 16 );

	auto X16 = m_IRBuilder.CreateLoad( &m_registerX );
	auto readAddressLow8 = OrAllValues( shiftedPB32, m_IRBuilder.CreateZExt( AddAllValues( operand16, X16 ), llvm::Type::getInt32Ty( m_LLVMContext ) ) );
	auto readAddressHigh8 = OrAllValues( shiftedPB32, m_IRBuilder.CreateZExt( AddAllValues( operand16, X16, GetConstant( 1, 16, false ) ), llvm::Type::getInt32Ty( m_LLVMContext ) ) );
	
	auto low8PC = Read8( readAddressLow8 );
	auto high8PC = Read8( readAddressHigh8 );

	auto[ pclow8Ptr, pcHigh8Ptr ] = GetLowHighPtrFromPtr16( &m_registerPC );

	m_IRBuilder.CreateStore( low8PC, pclow8Ptr );
	m_IRBuilder.CreateStore( high8PC, pcHigh8Ptr );

	auto jumpAddress = GetPBPC32();
	InsertJumpTable( jumpAddress, instructionOffset, functionName );
}

void Recompiler::PerformJumpIndirectLongInstruction( const uint32_t instructionOffset, llvm::Value* operand16, const std::string& functionName )
{
	auto low8PBPC = Read8( m_IRBuilder.CreateZExt( operand16, llvm::Type::getInt32Ty( m_LLVMContext ) ) );
	auto mid8PBPC = Read8( m_IRBuilder.CreateZExt( m_IRBuilder.CreateAdd( operand16, GetConstant( 1, 16, false ) ), llvm::Type::getInt32Ty( m_LLVMContext ) ) );
	auto high8PBPC = Read8( m_IRBuilder.CreateZExt( m_IRBuilder.CreateAdd( operand16, GetConstant( 2, 16, false ) ), llvm::Type::getInt32Ty( m_LLVMContext ) ) );
	
	auto pbpc32 = CombineTo32( low8PBPC, mid8PBPC, high8PBPC );
	InsertJumpTable( pbpc32, instructionOffset, functionName );
}

void Recompiler::InsertFunctionCall( const uint32_t instructionOffset )
{
	auto findFunctionNameResult = m_OffsetToFunctionName.find( instructionOffset );
	assert( findFunctionNameResult != m_OffsetToFunctionName.end() );

	const auto& functionName = findFunctionNameResult->second;
	auto findFunctionResult = m_Functions.find( functionName );
	assert( findFunctionResult != m_Functions.end() );
	auto function = findFunctionResult->second;
	m_IRBuilder.CreateCall( function );
}

void Recompiler::PerformCallShortInstruction( const uint32_t instructionOffset )
{
	auto pcPlus2 = AddAllValues( m_IRBuilder.CreateLoad( &m_registerPC ), GetConstant( 2, 16, false ) );
	auto [pcLow8, pcHigh8] = ConvertTo8( pcPlus2 );
	Push( pcHigh8 );
	Push( pcLow8 );

	InsertFunctionCall( instructionOffset );
}

void Recompiler::PerformCallLongInstruction( const uint32_t instructionOffset )
{
	auto pb8 = m_IRBuilder.CreateLoad( &m_registerPB );
	Push( pb8 );
	
	auto pcPlus3 = AddAllValues( m_IRBuilder.CreateLoad( &m_registerPC ), GetConstant( 3, 16, false ) );
	auto[ pcLow8, pcHigh8 ] = ConvertTo8( pcPlus3 );
	Push( pcHigh8 );
	Push( pcLow8 );

	PerformStackPointerEmulationFlagForcedConfiguration();

	InsertFunctionCall( instructionOffset );
}

void Recompiler::PerformCallIndexedIndirectInstruction( const uint32_t instructionOffset, llvm::Value* operand16 )
{
	auto pcPlus2 = AddAllValues( m_IRBuilder.CreateLoad( &m_registerPC ), GetConstant( 2, 16, false ) );
	auto[ pcLow8, pcHigh8 ] = ConvertTo8( pcPlus2 );
	PushNative( pcHigh8 );
	PushNative( pcLow8 );

	auto PB8 = m_IRBuilder.CreateLoad( &m_registerPB );
	auto PB32 = m_IRBuilder.CreateZExt( PB8, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto shiftedPB32 = m_IRBuilder.CreateShl( PB32, 16 );

	auto X16 = m_IRBuilder.CreateLoad( &m_registerX );
	auto readAddressLow8 = OrAllValues( shiftedPB32, m_IRBuilder.CreateZExt( AddAllValues( operand16, X16 ), llvm::Type::getInt32Ty( m_LLVMContext ) ) );
	auto readAddressHigh8 = OrAllValues( shiftedPB32, m_IRBuilder.CreateZExt( AddAllValues( operand16, X16, GetConstant( 1, 16, false ) ), llvm::Type::getInt32Ty( m_LLVMContext ) ) );

	auto low8PC = Read8( readAddressLow8 );
	auto high8PC = Read8( readAddressHigh8 );

	auto[ pclow8Ptr, pcHigh8Ptr ] = GetLowHighPtrFromPtr16( &m_registerPC );

	m_IRBuilder.CreateStore( low8PC, pclow8Ptr );
	m_IRBuilder.CreateStore( high8PC, pcHigh8Ptr );

	auto jumpAddress = GetPBPC32();
		
	PerformStackPointerEmulationFlagForcedConfiguration();

	auto findJumpTableEntries = m_JumpTables.find( instructionOffset );
	assert( findJumpTableEntries != m_JumpTables.end() );

	auto jumpTableEntries = findJumpTableEntries->second;
	const auto numSwitchCases = jumpTableEntries.size();
	auto endBlock = llvm::BasicBlock::Create( m_LLVMContext, "", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );
	auto panicBlock = llvm::BasicBlock::Create( m_LLVMContext, "PanicBlock", m_CurrentBasicBlock ? m_CurrentBasicBlock->getParent() : nullptr );

	auto sw = m_IRBuilder.CreateSwitch( jumpAddress, endBlock, numSwitchCases );
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
		auto caseValue = llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( jumpTableEntry.first ), false ) );
		sw->addCase( caseValue, caseBlock );
	}

	m_IRBuilder.SetInsertPoint( panicBlock );
	m_IRBuilder.CreateCall( m_PanicFunction );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformReturnInterruptInstruction()
{
	Pull();
	PerformProcessorStatusRegisterForcedConfiguration();
	Pull();

	auto EF = m_IRBuilder.CreateLoad( &m_EmulationFlag );
	auto EFCond = m_IRBuilder.CreateICmpEQ( EF, GetConstant( 1, 1, false ) );
	auto [thenBlock, elseBlock, endBlock] = CreateCondTestThenElseBlock( EFCond );
	
	SelectBlock( thenBlock );
	Pull();
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( elseBlock );
	Pull();
	Pull();
	m_IRBuilder.CreateBr( endBlock );
	
	SelectBlock( endBlock );
	m_IRBuilder.CreateRetVoid();
	m_CurrentBasicBlock = nullptr;
}

void Recompiler::PerformReturnShortInstruction()
{
	Pull();
	Pull();
	
	m_IRBuilder.CreateRetVoid();
	m_CurrentBasicBlock = nullptr;
}

void Recompiler::PerformReturnLongInstruction()
{
	PullNative();
	PullNative();
	PullNative();

	PerformStackPointerEmulationFlagForcedConfiguration();
	
	m_IRBuilder.CreateRetVoid();
	m_CurrentBasicBlock = nullptr;
}

void Recompiler::PerformPullDInstruction()
{
	auto[ low8Ptr, high8Ptr ] = Recompiler::GetLowHighPtrFromPtr16( &m_registerDP );

	auto low = PullNative();
	m_IRBuilder.CreateStore( low, low8Ptr );
	auto high = PullNative();
	m_IRBuilder.CreateStore( high, high8Ptr );

	auto newValue16 = m_IRBuilder.CreateLoad( &m_registerDP );
	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( newValue16, GetConstant( 0, 16, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits16( newValue16, 0x8000 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	PerformStackPointerEmulationFlagForcedConfiguration();
}

void Recompiler::PerformPullBInstruction()
{
	auto value = Pull();
	m_IRBuilder.CreateStore( value, &m_registerDB );
	
	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( value, GetConstant( 0, 8, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits8( value, 0x80 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );
}

void Recompiler::PerformPullPInstruction()
{
	auto value = Pull();
	SetProcessorStatusFlagsFromValue( value );
	
	PerformProcessorStatusRegisterForcedConfiguration();
}

void Recompiler::PerformPushEffectiveAddressInstruction( llvm::Value* operand16 )
{
	auto[ low8, high8 ] = ConvertTo8( operand16 );
	PushNative( high8 );
	PushNative( low8 );
	PerformStackPointerEmulationFlagForcedConfiguration();
}

void Recompiler::PerformPushEffectiveIndirectAddressInstruction( llvm::Value* address32 )
{
	auto low8 = ReadDirectNative( address32 );
	auto high8 = ReadDirectNative( m_IRBuilder.CreateAdd( address32, GetConstant( 1, 32, false ) ) );
	PushNative( high8 );
	PushNative( low8 );
	PerformStackPointerEmulationFlagForcedConfiguration();
}

void Recompiler::PerformPushEffectiveRelativeAddressInstruction( llvm::Value* operand16 )
{
	auto pbpc = GetPBPC32();

	auto result32 = m_IRBuilder.CreateAdd( pbpc, m_IRBuilder.CreateZExt( operand16, llvm::Type::getInt32Ty( m_LLVMContext ) ) );
	auto result16 = m_IRBuilder.CreateTrunc( result32, llvm::Type::getInt16Ty( m_LLVMContext ) );
	auto[ low8, high8 ] = ConvertTo8( result16 );
	PushNative( high8 );
	PushNative( low8 );

	PerformStackPointerEmulationFlagForcedConfiguration();
}

void Recompiler::PerformBlockMoveInstruction( RegisterModeFlag modeFlag, llvm::Value* operand32, llvm::Value* adjust16 )
{
	auto sourceBank32 = m_IRBuilder.CreateLShr( m_IRBuilder.CreateAnd( operand32, 0xff00 ), 8 );
	auto destinationBank32 = m_IRBuilder.CreateAnd( operand32, 0xff );	

	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );

	SelectBlock( flagSetBlock );
	auto adjust8 = m_IRBuilder.CreateTrunc( adjust16, llvm::Type::getInt8Ty( m_LLVMContext ) );
	InstructionBlockMove8( sourceBank32, destinationBank32, adjust8, flagSetBlock, endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionBlockMove16( sourceBank32, destinationBank32, adjust16, flagNotSetBlock, endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformPullInstruction( RegisterModeFlag modeFlag, llvm::Value* register16Ptr )
{
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );

	SelectBlock( flagSetBlock );
	InstructionPull8( register16Ptr );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionPull16( register16Ptr );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::InstructionImpliedModify8( Operation op, llvm::Value* ptr16 )
{
	auto [low8Ptr, high8Ptr ] = GetLowHighPtrFromPtr16( ptr16 );
	auto currentValue = m_IRBuilder.CreateLoad( low8Ptr );
	auto newValue = ( this->*op )( currentValue );
	m_IRBuilder.CreateStore( newValue, low8Ptr );
}

void Recompiler::InstructionImpliedModify16( Operation op, llvm::Value* ptr16 )
{
	auto currentValue = m_IRBuilder.CreateLoad( ptr16 );
	auto newValue = ( this->*op )( currentValue );
	m_IRBuilder.CreateStore( newValue, ptr16 );
}

void Recompiler::InstructionBankModify8( Operation op, llvm::Value* address32 )
{
	auto read = ReadBank( address32 );
	auto result = ( this->*op )( read );
	WriteBank( address32, result );
}

llvm::Value* Recompiler::CombineTo16( llvm::Value* low8, llvm::Value* high8 )
{
	auto low16 = m_IRBuilder.CreateZExt( low8, llvm::Type::getInt16Ty( m_LLVMContext ) );
	auto high16 = m_IRBuilder.CreateZExt( high8, llvm::Type::getInt16Ty( m_LLVMContext ) );
	auto high16Shifted = m_IRBuilder.CreateShl( high16, 8 );
	return m_IRBuilder.CreateOr( low16, high16Shifted );
}

llvm::Value* Recompiler::CombineTo32( llvm::Value* low8, llvm::Value* mid8, llvm::Value* high8 )
{
	auto low32 = m_IRBuilder.CreateZExt( low8, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto mid32 = m_IRBuilder.CreateZExt( mid8, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto high32 = m_IRBuilder.CreateZExt( high8, llvm::Type::getInt32Ty( m_LLVMContext ) );

	auto mid32Shifted = m_IRBuilder.CreateShl( mid32, 8 );
	auto high32Shifted = m_IRBuilder.CreateShl( high32, 16 );
	return m_IRBuilder.CreateOr( low32, m_IRBuilder.CreateOr( mid32Shifted, high32Shifted ) );
}

void Recompiler::PerformDirectModifyInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address32 )
{
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );

	SelectBlock( flagSetBlock );
	InstructionDirectModify8( op8, address32 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionDirectModify16( op16, address32 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformBankModifyInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address32 )
{
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );

	SelectBlock( flagSetBlock );
	InstructionBankModify8( op8, address32 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionBankModify16( op16, address32 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformBankReadInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address32 )
{
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );

	SelectBlock( flagSetBlock );
	InstructionBankRead8( op8, address32 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionBankRead16( op16, address32 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformBankReadInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address16, llvm::Value* I )
{
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );

	SelectBlock( flagSetBlock );
	InstructionBankRead8( op8, address16, I );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionBankRead16( op16, address16, I );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformLongReadInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address32, llvm::Value* I )
{
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );

	SelectBlock( flagSetBlock );
	InstructionLongRead8( op8, address32, I );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionLongRead16( op16, address32, I );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformDirectReadInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address32 )
{
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );

	SelectBlock( flagSetBlock );
	InstructionDirectRead8( op8, address32 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionDirectRead16( op16, address32 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformDirectReadInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address16, llvm::Value* I16 )
{
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );

	SelectBlock( flagSetBlock );
	InstructionDirectRead8( op8, address16, I16 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionDirectRead16( op16, address16, I16 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformIndirectReadInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address32 )
{
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );

	SelectBlock( flagSetBlock );
	InstructionIndirectRead8( op8, address32 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionIndirectRead16( op16, address32 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformIndexedIndirectReadInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address32 )
{
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );

	SelectBlock( flagSetBlock );
	InstructionIndexedIndirectRead8( op8, address32 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionIndexedIndirectRead16( op16, address32 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformIndirectIndexedReadInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address32 )
{
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );

	SelectBlock( flagSetBlock );
	InstructionIndirectIndexedRead8( op8, address32 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionIndirectIndexedRead16( op16, address32 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformIndirectLongReadInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address32, llvm::Value* I16 )
{
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );

	SelectBlock( flagSetBlock );
	InstructionIndirectLongRead8( op8, address32, I16 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionIndirectLongRead16( op16, address32, I16 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformStackReadInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address32 )
{
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );

	SelectBlock( flagSetBlock );
	InstructionStackRead8( op8, address32 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionStackRead16( op16, address32 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformIndirectStackReadInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address32 )
{
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );

	SelectBlock( flagSetBlock );
	InstructionIndirectStackRead8( op8, address32 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionIndirectStackRead16( op16, address32 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformBankWriteInstruction( RegisterModeFlag modeFlag, llvm::Value* address32, llvm::Value* value16 )
{
	auto[ low8, high8 ] = ConvertTo8( value16 );
	
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );
	
	SelectBlock( flagSetBlock );
	InstructionBankWrite8( address32, low8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionBankWrite16( address32, low8, high8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformBankWriteInstruction( RegisterModeFlag modeFlag, llvm::Value* address32, llvm::Value* I16, llvm::Value* value16 )
{
	auto[ low8, high8 ] = ConvertTo8( value16 );
	
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );
	
	SelectBlock( flagSetBlock );
	InstructionBankWrite8( address32, I16, low8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionBankWrite16( address32, I16, low8, high8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformLongWriteInstruction( RegisterModeFlag modeFlag, llvm::Value* address32, llvm::Value* I16 )
{
	auto A = m_IRBuilder.CreateLoad( &m_registerA );
	auto[ low8, high8 ] = ConvertTo8( A );
	
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );

	SelectBlock( flagSetBlock );
	InstructionLongWrite8( address32, I16, low8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionLongWrite16( address32, I16, low8, high8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformDirectWriteInstruction( RegisterModeFlag modeFlag, llvm::Value* address32, llvm::Value* value16 )
{
	auto[ low8, high8 ] = ConvertTo8( value16 );
	
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );

	SelectBlock( flagSetBlock );
	InstructionDirectWrite8( address32, low8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionDirectWrite16( address32, low8, high8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformDirectWriteInstruction( RegisterModeFlag modeFlag, llvm::Value* address32, llvm::Value* I16, llvm::Value* value16 )
{
	auto[ low8, high8 ] = ConvertTo8( value16 );
	
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );

	SelectBlock( flagSetBlock );
	InstructionDirectWrite8( address32, I16, low8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionDirectWrite16( address32, I16, low8, high8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformIndirectWriteInstruction( RegisterModeFlag modeFlag, llvm::Value* address32 )
{
	auto A = m_IRBuilder.CreateLoad( &m_registerA );
	auto[ low8, high8 ] = ConvertTo8( A );
	
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );

	SelectBlock( flagSetBlock );
	InstructionIndirectWrite8( address32, low8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionIndirectWrite16( address32, low8, high8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformIndexedIndirectWriteInstruction( RegisterModeFlag modeFlag, llvm::Value* address32 )
{
	auto A = m_IRBuilder.CreateLoad( &m_registerA );
	auto[ low8, high8 ] = ConvertTo8( A );
	
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );
	
	SelectBlock( flagSetBlock );
	InstructionIndexedIndirectWrite8( address32, low8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionIndexedIndirectWrite16( address32, low8, high8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformIndirectIndexedWriteInstruction( RegisterModeFlag modeFlag, llvm::Value* address32 )
{
	auto A = m_IRBuilder.CreateLoad( &m_registerA );
	auto[ low8, high8 ] = ConvertTo8( A );
	
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );

	SelectBlock( flagSetBlock );
	InstructionIndirectIndexedWrite8( address32, low8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionIndirectIndexedWrite16( address32, low8, high8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformIndirectLongWriteInstruction( RegisterModeFlag modeFlag, llvm::Value* address32, llvm::Value* I16 )
{	
	auto A = m_IRBuilder.CreateLoad( &m_registerA );
	auto[ low8, high8 ] = ConvertTo8( A );
	
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );

	SelectBlock( flagSetBlock );
	InstructionIndirectLongWrite8( address32, I16, low8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionIndirectLongWrite16( address32, I16, low8, high8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformStackWriteInstruction( RegisterModeFlag modeFlag, llvm::Value* address32 )
{	
	auto A = m_IRBuilder.CreateLoad( &m_registerA );
	auto[ low8, high8 ] = ConvertTo8( A );
	
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );
	
	SelectBlock( flagSetBlock );
	InstructionStackWrite8( address32, low8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionStackWrite16( address32, low8, high8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformBitImmediateInstruction( RegisterModeFlag modeFlag, llvm::Value* operand16 )
{
	auto[ low8, high8 ] = ConvertTo8( operand16 );
	
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );
	
	SelectBlock( flagSetBlock );
	InstructionBitImmediate8( low8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionBitImmediate16( operand16 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformIndirectStackWriteInstruction( RegisterModeFlag modeFlag, llvm::Value* address32 )
{
	auto A = m_IRBuilder.CreateLoad( &m_registerA );
	auto[ low8, high8 ] = ConvertTo8( A );
	
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );
	
	SelectBlock( flagSetBlock );
	InstructionIndirectStackWrite8( address32, low8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionIndirectStackWrite16( address32, low8, high8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::InstructionBankModify16( Operation op, llvm::Value* address32 )
{
	auto readLow8 = ReadBank( address32 );
	auto address32High = m_IRBuilder.CreateAdd( address32, GetConstant( 1, 32, false ) );
	auto readHigh8 = ReadBank( address32High );

	auto read16 = CombineTo16( readLow8, readHigh8 );

	auto result16 = ( this->*op )( read16 );

	auto[ resultLow8, resultHigh8 ] = ConvertTo8( result16 );

	WriteBank( address32High, resultHigh8 );
	WriteBank( address32, resultLow8 );
}

void Recompiler::InstructionBankRead8( Operation op, llvm::Value* address32 )
{
	auto read = ReadBank( address32 );
	( this->*op )( read );
}

void Recompiler::InstructionBankRead16( Operation op, llvm::Value* address32 )
{
	auto readLow8 = ReadBank( address32 );
	auto address32High = m_IRBuilder.CreateAdd( address32, GetConstant( 1, 32, false ) );
	auto readHigh8 = ReadBank( address32High );

	auto read16 = CombineTo16( readLow8, readHigh8 );

	( this->*op )( read16 );
}

void Recompiler::InstructionBankRead8( Operation op, llvm::Value* address16, llvm::Value* I16 )
{
	auto finalAddress = m_IRBuilder.CreateZExt( m_IRBuilder.CreateAdd( address16, I16 ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto read = ReadBank( finalAddress );
	( this->*op )( read );
}

void Recompiler::InstructionBankRead16( Operation op, llvm::Value* address16, llvm::Value* I16 )
{
	auto address32Low = m_IRBuilder.CreateZExt( m_IRBuilder.CreateAdd( address16, I16 ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto address32High = m_IRBuilder.CreateAdd( address32Low, GetConstant( 1, 32, false ) );
	auto readLow8 = ReadBank( address32Low );
	auto readHigh8 = ReadBank( address32High );

	auto read16 = CombineTo16( readLow8, readHigh8 );

	( this->*op )( read16 );
}

void Recompiler::InstructionLongRead8( Operation op, llvm::Value* address32, llvm::Value* I16 )
{
	auto I32 = m_IRBuilder.CreateZExt( I16, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto finalAddress = m_IRBuilder.CreateAdd( address32, I32 );
	auto read = ReadLong( finalAddress );
	( this->*op )( read );
}

void Recompiler::InstructionLongRead16( Operation op, llvm::Value* address32, llvm::Value* I16 )
{
	auto I32 = m_IRBuilder.CreateZExt( I16, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto address32Low = m_IRBuilder.CreateZExt( m_IRBuilder.CreateAdd( address32, I32 ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto address32High = m_IRBuilder.CreateAdd( address32Low, GetConstant( 1, 32, false ) );
	auto readLow8 = ReadLong( address32Low );
	auto readHigh8 = ReadLong( address32High );

	auto read16 = CombineTo16( readLow8, readHigh8 );

	( this->*op )( read16 );
}

void Recompiler::InstructionDirectRead8( Operation op, llvm::Value* address32 )
{
	auto read = ReadDirect( address32 );
	( this->*op )( read );
}

void Recompiler::InstructionDirectRead16( Operation op, llvm::Value* address32 )
{
	auto readLow8 = ReadDirect( address32 );
	auto address32High = m_IRBuilder.CreateAdd( address32, GetConstant( 1, 32, false ) );
	auto readHigh8 = ReadDirect( address32High );

	auto read16 = CombineTo16( readLow8, readHigh8 );

	( this->*op )( read16 );
}

void Recompiler::InstructionDirectRead8( Operation op, llvm::Value* address16, llvm::Value* I16 )
{
	auto finalAddress = m_IRBuilder.CreateZExt( m_IRBuilder.CreateAdd( address16, I16 ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto read = ReadDirect( finalAddress );
	( this->*op )( read );
}

void Recompiler::InstructionDirectRead16( Operation op, llvm::Value* address16, llvm::Value* I16 )
{
	auto address32Low = m_IRBuilder.CreateZExt( m_IRBuilder.CreateAdd( address16, I16 ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto address32High = m_IRBuilder.CreateAdd( address32Low, GetConstant( 1, 32, false ) );
	auto readLow8 = ReadDirect( address32Low );
	auto readHigh8 = ReadDirect( address32High );

	auto read16 = CombineTo16( readLow8, readHigh8 );

	( this->*op )( read16 );
}

void Recompiler::InstructionIndirectRead8( Operation op, llvm::Value* address32 )
{
	auto readLow8 = ReadDirect( address32 );
	auto address32High = m_IRBuilder.CreateAdd( address32, GetConstant( 1, 32, false ) );
	auto readHigh8 = ReadDirect( address32High );

	auto bankAddress = m_IRBuilder.CreateZExt( CombineTo16( readLow8, readHigh8 ), llvm::Type::getInt32Ty( m_LLVMContext ) );

	auto read8 = ReadBank( bankAddress );
	( this->*op )( read8 );
}

void Recompiler::InstructionIndirectRead16( Operation op, llvm::Value* address32 )
{
	auto readDirectLow8 = ReadDirect( address32 );
	auto address32High = m_IRBuilder.CreateAdd( address32, GetConstant( 1, 32, false ) );
	auto readDirectHigh8 = ReadDirect( address32High );

	auto bankAddress = m_IRBuilder.CreateZExt( CombineTo16( readDirectLow8, readDirectHigh8 ), llvm::Type::getInt32Ty( m_LLVMContext ) );

	auto readLow8 = ReadBank( bankAddress );
	auto readHigh8 = ReadBank( m_IRBuilder.CreateAdd( bankAddress, GetConstant( 1, 32, false ) ) );

	auto read16 = CombineTo16( readLow8, readHigh8 );

	( this->*op )( read16 );
}

void Recompiler::InstructionIndexedIndirectRead8( Operation op, llvm::Value* address32 )
{
	auto X = m_IRBuilder.CreateLoad( &m_registerX );
	auto X32 = m_IRBuilder.CreateZExt( X, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto readLowDirectAddress = m_IRBuilder.CreateAdd( address32, X32 );
	auto readDirectLow8 = ReadDirect( readLowDirectAddress );
	auto readHighDirectAddress = m_IRBuilder.CreateAdd( readLowDirectAddress, GetConstant( 1, 32, false ) );
	auto readDirectHigh8 = ReadDirect( readHighDirectAddress );

	auto bankAddress = m_IRBuilder.CreateZExt( CombineTo16( readDirectLow8, readDirectHigh8 ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto read8 = ReadBank( bankAddress );
	( this->*op )( read8 );
}

void Recompiler::InstructionIndexedIndirectRead16( Operation op, llvm::Value* address32 )
{
	auto X = m_IRBuilder.CreateLoad( &m_registerX );
	auto X32 = m_IRBuilder.CreateZExt( X, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto readLowDirectAddress = m_IRBuilder.CreateAdd( address32, X32 );
	auto readDirectLow8 = ReadDirect( readLowDirectAddress );
	auto readHighDirectAddress = m_IRBuilder.CreateAdd( readLowDirectAddress, GetConstant( 1, 32, false ) );
	auto readDirectHigh8 = ReadDirect( readHighDirectAddress );

	auto bankAddress = m_IRBuilder.CreateZExt( CombineTo16( readDirectLow8, readDirectHigh8 ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto readLow8 = ReadBank( bankAddress );
	auto readHigh8 = ReadBank( m_IRBuilder.CreateAdd( bankAddress, GetConstant( 1, 32, false ) ) );

	auto read16 = CombineTo16( readLow8, readHigh8 );

	( this->*op )( read16 );
}

void Recompiler::InstructionIndirectIndexedRead8( Operation op, llvm::Value* address32 )
{
	auto readDirectLow8 = ReadDirect( address32 );
	auto readDirectHigh8 = ReadDirect( m_IRBuilder.CreateAdd( address32, GetConstant( 1, 32, false ) ) );

	auto bankAddress = m_IRBuilder.CreateZExt( CombineTo16( readDirectLow8, readDirectHigh8 ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto Y = m_IRBuilder.CreateLoad( &m_registerY );
	auto Y32 = m_IRBuilder.CreateZExt( Y, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto indexedBankAndress = m_IRBuilder.CreateAdd( bankAddress, Y32 );
	auto read8 = ReadBank( indexedBankAndress );
	( this->*op )( read8 );
}

void Recompiler::InstructionIndirectIndexedRead16( Operation op, llvm::Value* address32 )
{
	auto readDirectLow8 = ReadDirect( address32 );
	auto readDirectHigh8 = ReadDirect( m_IRBuilder.CreateAdd( address32, GetConstant( 1, 32, false ) ) );

	auto bankAddress = m_IRBuilder.CreateZExt( CombineTo16( readDirectLow8, readDirectHigh8 ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto Y = m_IRBuilder.CreateLoad( &m_registerY );
	auto Y32 = m_IRBuilder.CreateZExt( Y, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto indexedBankAndress = m_IRBuilder.CreateAdd( bankAddress, Y32 );


	auto readLow8 = ReadBank( indexedBankAndress );
	auto readHigh8 = ReadBank( m_IRBuilder.CreateAdd( indexedBankAndress, GetConstant( 1, 32, false ) ) );

	auto read16 = CombineTo16( readLow8, readHigh8 );

	( this->*op )( read16 );
}

void Recompiler::InstructionIndirectLongRead8( Operation op, llvm::Value* address32, llvm::Value* I16 )
{
	auto low8 = ReadDirectNative( address32 );
	auto mid8 = ReadDirectNative( m_IRBuilder.CreateAdd( address32, GetConstant( 1, 32, false ) ) );
	auto high8 = ReadDirectNative( m_IRBuilder.CreateAdd( address32, GetConstant( 2, 32, false ) ) );
	
	auto longAddress = CombineTo32( low8, mid8, high8 );
	auto I32 = m_IRBuilder.CreateZExt( I16, llvm::Type::getInt32Ty( m_LLVMContext ) );

	auto read8 = ReadLong( m_IRBuilder.CreateAdd( longAddress, I32 ) );
	( this->*op )( read8 );
}

void Recompiler::InstructionIndirectLongRead16( Operation op, llvm::Value* address32, llvm::Value* I16 )
{
	auto low8 = ReadDirectNative( address32 );
	auto mid8 = ReadDirectNative( m_IRBuilder.CreateAdd( address32, GetConstant( 1, 32, false ) ) );
	auto high8 = ReadDirectNative( m_IRBuilder.CreateAdd( address32, GetConstant( 2, 32, false ) ) );

	auto longAddress = CombineTo32( low8, mid8, high8 );
	auto I32 = m_IRBuilder.CreateZExt( I16, llvm::Type::getInt32Ty( m_LLVMContext ) );

	auto indexedLongAddress = m_IRBuilder.CreateAdd( longAddress, I32 );
	auto readLow8 = ReadLong( indexedLongAddress );
	auto readHigh8 = ReadLong( m_IRBuilder.CreateAdd( indexedLongAddress, GetConstant( 1, 32, false ) ) );

	auto read16 = CombineTo16( readLow8, readHigh8 );
	
	( this->*op )( read16 );
}

void Recompiler::InstructionStackRead8( Operation op, llvm::Value* address32 )
{
	auto read = ReadStack( address32 );
	( this->*op )( read );
}

void Recompiler::InstructionStackRead16( Operation op, llvm::Value* address32 )
{
	auto readLow8 = ReadStack( address32 );
	auto address32High = m_IRBuilder.CreateAdd( address32, GetConstant( 1, 32, false ) );
	auto readHigh8 = ReadStack( address32High );

	auto read16 = CombineTo16( readLow8, readHigh8 );

	( this->*op )( read16 );
}

void Recompiler::InstructionIndirectStackRead8( Operation op, llvm::Value* address32 )
{
	auto readStackLow8 = ReadStack( address32 );
	auto address32High = m_IRBuilder.CreateAdd( address32, GetConstant( 1, 32, false ) );
	auto readStackHigh8 = ReadStack( address32High );

	auto bankAddress = m_IRBuilder.CreateZExt( CombineTo16( readStackLow8, readStackHigh8 ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto Y = m_IRBuilder.CreateLoad( &m_registerY );
	auto Y32 = m_IRBuilder.CreateZExt( Y, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto indexedBankAndress = m_IRBuilder.CreateAdd( bankAddress, Y32 );

	auto read8 = ReadBank( indexedBankAndress );
	( this->*op )( read8 );
}

void Recompiler::InstructionIndirectStackRead16( Operation op, llvm::Value* address32 )
{
	auto readStackLow8 = ReadStack( address32 );
	auto address32High = m_IRBuilder.CreateAdd( address32, GetConstant( 1, 32, false ) );
	auto readStackHigh8 = ReadStack( address32High );

	auto bankAddress = m_IRBuilder.CreateZExt( CombineTo16( readStackLow8, readStackHigh8 ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto Y = m_IRBuilder.CreateLoad( &m_registerY );
	auto Y32 = m_IRBuilder.CreateZExt( Y, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto indexedBankAndress = m_IRBuilder.CreateAdd( bankAddress, Y32 );

	auto readLow8 = ReadBank( indexedBankAndress );
	auto readHigh8 = ReadBank( m_IRBuilder.CreateAdd( indexedBankAndress, GetConstant( 1, 32, false ) ) );

	auto read16 = CombineTo16( readLow8, readHigh8 );
	( this->*op )( read16 );
}

void Recompiler::InstructionBankIndexedModify8( Operation op, llvm::Value* address16 )
{
	auto X = m_IRBuilder.CreateLoad( &m_registerX );
	auto readBankAddress32 = m_IRBuilder.CreateZExt( m_IRBuilder.CreateAdd( X, address16 ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto read = ReadBank( readBankAddress32 );
	auto result = ( this->*op )( read );
	WriteBank( readBankAddress32, result );
}

void Recompiler::InstructionBankIndexedModify16( Operation op, llvm::Value* address16 )
{
	auto X = m_IRBuilder.CreateLoad( &m_registerX );
	auto readBankAddressLow32 = m_IRBuilder.CreateZExt( m_IRBuilder.CreateAdd( X, address16 ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto readBankAddressHigh32 = m_IRBuilder.CreateAdd( readBankAddressLow32, GetConstant( 1, 32, false ) );
	auto readLow8 = ReadBank( readBankAddressLow32 );
	auto readHigh8 = ReadBank( readBankAddressHigh32 );

	auto read16 = CombineTo16( readLow8, readHigh8 );

	auto result16 = ( this->*op )( read16 );

	auto[ resultLow8, resultHigh8 ] = ConvertTo8( result16 );

	WriteBank( readBankAddressHigh32, resultHigh8 );
	WriteBank( readBankAddressLow32, resultLow8 );
}

void Recompiler::InstructionBankWrite8( llvm::Value* address32, llvm::Value* value )
{
	WriteBank( address32, value );
}

void Recompiler::InstructionBankWrite16( llvm::Value* address32, llvm::Value* low8, llvm::Value* high8 )
{
	WriteBank( address32, low8 );
	WriteBank( m_IRBuilder.CreateAdd( address32, GetConstant( 1, 32, false ) ), high8 );
}

void Recompiler::InstructionBankWrite8( llvm::Value* address32, llvm::Value* I16, llvm::Value* value )
{
	llvm::Value* I32 = m_IRBuilder.CreateZExt( I16, llvm::Type::getInt32Ty( m_LLVMContext ) );
	WriteBank( m_IRBuilder.CreateAdd( address32, I32 ), value );
}

void Recompiler::InstructionBankWrite16( llvm::Value* address32, llvm::Value* I16, llvm::Value* low8, llvm::Value* high8 )
{
	llvm::Value* I32 = m_IRBuilder.CreateZExt( I16, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto low8WriteAddress = m_IRBuilder.CreateAdd( address32, I32 );
	WriteBank( low8WriteAddress, low8 );
	WriteBank( m_IRBuilder.CreateAdd( low8WriteAddress, GetConstant( 1, 32, false ) ), high8 );
}

void Recompiler::InstructionLongWrite8( llvm::Value* address32, llvm::Value* I16, llvm::Value* value )
{
	llvm::Value* I32 = m_IRBuilder.CreateZExt( I16, llvm::Type::getInt32Ty( m_LLVMContext ) );
	WriteLong( m_IRBuilder.CreateAdd( address32, I32 ), value );
}

void Recompiler::InstructionLongWrite16( llvm::Value* address32, llvm::Value* I16, llvm::Value* low8, llvm::Value* high8 )
{
	llvm::Value* I32 = m_IRBuilder.CreateZExt( I16, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto low8WriteAddress = m_IRBuilder.CreateAdd( address32, I32 );
	WriteLong( low8WriteAddress, low8 );
	WriteLong( m_IRBuilder.CreateAdd( low8WriteAddress, GetConstant( 1, 32, false ) ), high8 );
}

void Recompiler::InstructionDirectWrite8( llvm::Value* address32, llvm::Value* value )
{
	WriteDirect( address32, value );
}
void Recompiler::InstructionDirectWrite16( llvm::Value* address32, llvm::Value* low8, llvm::Value* high8 )
{
	WriteDirect( address32, low8 );
	WriteDirect( m_IRBuilder.CreateAdd( address32, GetConstant( 1, 32, false ) ), high8 );
}

void Recompiler::InstructionDirectWrite8( llvm::Value* address32, llvm::Value* I16, llvm::Value* value )
{
	llvm::Value* I32 = m_IRBuilder.CreateZExt( I16, llvm::Type::getInt32Ty( m_LLVMContext ) );
	WriteDirect( m_IRBuilder.CreateAdd( address32, I32 ), value );
}

void Recompiler::InstructionDirectWrite16( llvm::Value* address32, llvm::Value* I16, llvm::Value* low8, llvm::Value* high8 )
{
	llvm::Value* I32 = m_IRBuilder.CreateZExt( I16, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto low8WriteAddress = m_IRBuilder.CreateAdd( address32, I32 );
	WriteDirect( low8WriteAddress, low8 );
	WriteDirect( m_IRBuilder.CreateAdd( low8WriteAddress, GetConstant( 1, 32, false ) ), high8 );
}

void Recompiler::InstructionIndirectWrite8( llvm::Value* address32, llvm::Value* value )
{
	auto readDirectLow8 = ReadDirect( address32 );
	auto readDirectHigh8 = ReadDirect( m_IRBuilder.CreateAdd( address32, GetConstant( 1, 32, false ) ) );

	auto read16 = CombineTo16( readDirectLow8, readDirectHigh8 );
	auto writeAddress = m_IRBuilder.CreateZExt( read16, llvm::Type::getInt32Ty( m_LLVMContext ) );
	WriteBank( writeAddress, value );
}

void Recompiler::InstructionIndirectWrite16( llvm::Value* address32, llvm::Value* low8, llvm::Value* high8 )
{
	auto readDirectLow8 = ReadDirect( address32 );
	auto readDirectHigh8 = ReadDirect( m_IRBuilder.CreateAdd( address32, GetConstant( 1, 32, false ) ) );

	auto read16 = CombineTo16( readDirectLow8, readDirectHigh8 );
	auto writeAddress = m_IRBuilder.CreateZExt( read16, llvm::Type::getInt32Ty( m_LLVMContext ) );
	WriteBank( writeAddress, low8 );
	WriteBank( m_IRBuilder.CreateAdd( writeAddress, GetConstant( 1, 32, false ) ), high8 );
}

void Recompiler::InstructionIndexedIndirectWrite8( llvm::Value* address32, llvm::Value* value )
{
	auto X16 = m_IRBuilder.CreateLoad( &m_registerX );
	auto X32 = m_IRBuilder.CreateZExt( X16, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto readDirectAddressLow = m_IRBuilder.CreateAdd( address32, X32 );

	auto readDirectLow8 = ReadDirect( readDirectAddressLow );
	auto readDirectHigh8 = ReadDirect( m_IRBuilder.CreateAdd( readDirectAddressLow, GetConstant( 1, 32, false ) ) );

	auto writeAddress16 = CombineTo16( readDirectLow8, readDirectHigh8 );
	auto writeAddress32 = m_IRBuilder.CreateZExt( writeAddress16, llvm::Type::getInt32Ty( m_LLVMContext ) );
	WriteBank( writeAddress32, value );
}

void Recompiler::InstructionIndexedIndirectWrite16( llvm::Value* address32, llvm::Value* low8, llvm::Value* high8 )
{
	auto X16 = m_IRBuilder.CreateLoad( &m_registerX );
	auto X32 = m_IRBuilder.CreateZExt( X16, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto readDirectAddressLow = m_IRBuilder.CreateAdd( address32, X32 );

	auto readDirectLow8 = ReadDirect( readDirectAddressLow );
	auto readDirectHigh8 = ReadDirect( m_IRBuilder.CreateAdd( readDirectAddressLow, GetConstant( 1, 32, false ) ) );

	auto writeAddress16 = CombineTo16( readDirectLow8, readDirectHigh8 );
	auto writeAddress32 = m_IRBuilder.CreateZExt( writeAddress16, llvm::Type::getInt32Ty( m_LLVMContext ) );
	WriteBank( writeAddress32, low8 );
	WriteBank( m_IRBuilder.CreateAdd( writeAddress32, GetConstant( 1, 32, false ) ), high8 );
}

void Recompiler::InstructionIndirectIndexedWrite8( llvm::Value* address32, llvm::Value* value )
{
	auto readDirectLow8 = ReadDirect( address32 );
	auto readDirectHigh8 = ReadDirect( m_IRBuilder.CreateAdd( address32, GetConstant( 1, 32, false ) ) );

	auto writeAddress16 = CombineTo16( readDirectLow8, readDirectHigh8 );
	auto writeAddress32 = m_IRBuilder.CreateZExt( writeAddress16, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto Y16 = m_IRBuilder.CreateLoad( &m_registerY );
	auto Y32 = m_IRBuilder.CreateZExt( Y16, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto writeAddressIndexed32 = m_IRBuilder.CreateAdd( writeAddress32, Y32 );
	WriteBank( writeAddressIndexed32, value );
}

void Recompiler::InstructionIndirectIndexedWrite16( llvm::Value* address32, llvm::Value* low8, llvm::Value* high8 )
{
	auto readDirectLow8 = ReadDirect( address32 );
	auto readDirectHigh8 = ReadDirect( m_IRBuilder.CreateAdd( address32, GetConstant( 1, 32, false ) ) );

	auto writeAddress16 = CombineTo16( readDirectLow8, readDirectHigh8 );
	auto writeAddress32 = m_IRBuilder.CreateZExt( writeAddress16, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto Y16 = m_IRBuilder.CreateLoad( &m_registerY );
	auto Y32 = m_IRBuilder.CreateZExt( Y16, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto writeAddressIndexed32 = m_IRBuilder.CreateAdd( writeAddress32, Y32 );
	WriteBank( writeAddressIndexed32, low8 );
	WriteBank( m_IRBuilder.CreateAdd( writeAddressIndexed32, GetConstant( 1, 32, false ) ), high8 );
}

void Recompiler::InstructionIndirectLongWrite8( llvm::Value* address32, llvm::Value* I16, llvm::Value* value )
{
	auto readDirectLow8 = ReadDirectNative( address32 );
	auto readDirectMid8 = ReadDirectNative( m_IRBuilder.CreateAdd( address32, GetConstant( 1, 32, false ) ) );
	auto readDirectHigh8 = ReadDirectNative( m_IRBuilder.CreateAdd( address32, GetConstant( 2, 32, false ) ) );

	auto longAddress = CombineTo32( readDirectLow8, readDirectMid8, readDirectHigh8 );
	auto I32 = m_IRBuilder.CreateZExt( I16, llvm::Type::getInt32Ty( m_LLVMContext ) );
	
	WriteLong( m_IRBuilder.CreateAdd( longAddress, I32 ), value );
}

void Recompiler::InstructionIndirectLongWrite16( llvm::Value* address32, llvm::Value* I16, llvm::Value* low8, llvm::Value* high8 )
{
	auto readDirectLow8 = ReadDirectNative( address32 );
	auto readDirectMid8 = ReadDirectNative( m_IRBuilder.CreateAdd( address32, GetConstant( 1, 32, false ) ) );
	auto readDirectHigh8 = ReadDirectNative( m_IRBuilder.CreateAdd( address32, GetConstant( 2, 32, false ) ) );

	auto longAddress = CombineTo32( readDirectLow8, readDirectMid8, readDirectHigh8 );
	auto I32 = m_IRBuilder.CreateZExt( I16, llvm::Type::getInt32Ty( m_LLVMContext ) );

	auto writeLowAddress = m_IRBuilder.CreateAdd( longAddress, I32 );
	WriteLong( writeLowAddress, low8 );
	WriteLong( m_IRBuilder.CreateAdd( writeLowAddress, GetConstant( 1, 32, false ) ), high8 );
}

void Recompiler::InstructionStackWrite8( llvm::Value* address32, llvm::Value* value )
{
	WriteStack( address32, value );
}

void Recompiler::InstructionStackWrite16( llvm::Value* address32, llvm::Value* low8, llvm::Value* high8 )
{
	WriteStack( address32, low8 );
	WriteStack( m_IRBuilder.CreateAdd( address32, GetConstant( 1, 32, false ) ), low8 );
}

void Recompiler::InstructionIndirectStackWrite8( llvm::Value* address32, llvm::Value* value )
{
	auto readStackLow8 = ReadStack( address32 );
	auto readStackHigh8 = ReadStack( m_IRBuilder.CreateAdd( address32, GetConstant( 1, 32, false ) ) );

	auto writeAddress16 = CombineTo16( readStackLow8, readStackHigh8 );
	auto writeAddress32 = m_IRBuilder.CreateZExt( writeAddress16, llvm::Type::getInt32Ty( m_LLVMContext ) );

	auto Y16 = m_IRBuilder.CreateLoad( &m_registerY );	
	auto Y32 = m_IRBuilder.CreateZExt( Y16, llvm::Type::getInt32Ty( m_LLVMContext ) );

	WriteBank( m_IRBuilder.CreateAdd( writeAddress32, Y32 ), value );
}

void Recompiler::InstructionIndirectStackWrite16( llvm::Value* address32, llvm::Value* low8, llvm::Value* high8 )
{
	auto readStackLow8 = ReadStack( address32 );
	auto readStackHigh8 = ReadStack( m_IRBuilder.CreateAdd( address32, GetConstant( 1, 32, false ) ) );

	auto writeAddress16 = CombineTo16( readStackLow8, readStackHigh8 );
	auto writeAddress32 = m_IRBuilder.CreateZExt( writeAddress16, llvm::Type::getInt32Ty( m_LLVMContext ) );

	auto Y16 = m_IRBuilder.CreateLoad( &m_registerY );
	auto Y32 = m_IRBuilder.CreateZExt( Y16, llvm::Type::getInt32Ty( m_LLVMContext ) );
	
	auto writeAddressIndexed32 = m_IRBuilder.CreateAdd( writeAddress32, Y32 );

	WriteBank( writeAddressIndexed32, low8 );
	WriteBank( m_IRBuilder.CreateAdd( writeAddressIndexed32, GetConstant( 1, 32, false ) ), high8 );
}

void Recompiler::InstructionBitImmediate8( llvm::Value* operand8 )
{
	auto A = m_IRBuilder.CreateLoad( &m_registerA );
	auto[ ALow8, AHigh8 ] = ConvertTo8( A );

	auto result = m_IRBuilder.CreateAnd( operand8, ALow8 );
	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( result, GetConstant( 0, 8, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );
}

void Recompiler::InstructionBitImmediate16( llvm::Value* operand16 )
{
	auto A = m_IRBuilder.CreateLoad( &m_registerA );

	auto result = m_IRBuilder.CreateAnd( operand16, A );
	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( result, GetConstant( 0, 16, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );
}

void Recompiler::InstructionTransfer8( llvm::Value* sourceRegisterPtr, llvm::Value* destinationRegisterPtr )
{
	auto [ sourceLow8Ptr, sourceHigh8Ptr ] = GetLowHighPtrFromPtr16( sourceRegisterPtr );
	auto [ destinationLow8Ptr, destinationHigh8Ptr ] = GetLowHighPtrFromPtr16( destinationRegisterPtr );

	auto sourceLow8 = m_IRBuilder.CreateLoad( sourceLow8Ptr );
	m_IRBuilder.CreateStore( sourceLow8, destinationLow8Ptr );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( sourceLow8, GetConstant( 0, 8, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits8( sourceLow8, 0x80 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );
}

void Recompiler::InstructionTransfer16( llvm::Value* sourceRegisterPtr, llvm::Value* destinationRegisterPtr )
{
	auto source16Value = m_IRBuilder.CreateLoad( sourceRegisterPtr );
	m_IRBuilder.CreateStore( source16Value, destinationRegisterPtr );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( source16Value, GetConstant( 0, 16, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits16( source16Value, 0x8000 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );
}

void Recompiler::InstructionTransferSX8()
{
	auto[ spLow8Ptr, spHigh8Ptr ] = GetLowHighPtrFromPtr16( &m_registerSP );
	auto[ xLow8Ptr, xHigh8Ptr ] = GetLowHighPtrFromPtr16( &m_registerX );
	
	auto spLow8Value = m_IRBuilder.CreateLoad( spLow8Ptr );
	m_IRBuilder.CreateStore( spLow8Value, xLow8Ptr );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( spLow8Value, GetConstant( 0, 8, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits8( spLow8Value, 0x80 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );
}

void Recompiler::InstructionTransferSX16()
{
	auto sp16Value = m_IRBuilder.CreateLoad( &m_registerSP );
	m_IRBuilder.CreateStore( sp16Value, &m_registerX );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( sp16Value, GetConstant( 0, 16, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits16( sp16Value, 0x8000 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );
}

void Recompiler::InstructionPush8( llvm::Value* value8 )
{
	Push( value8 );
}

void Recompiler::InstructionPush16( llvm::Value* low8, llvm::Value* high8 )
{
	Push( high8 );
	Push( low8 );
}

void Recompiler::InstructionPull8( llvm::Value* register16Ptr )
{
	auto[ low8Ptr, high8Ptr ] = Recompiler::GetLowHighPtrFromPtr16( register16Ptr );
	auto value = Pull();
	m_IRBuilder.CreateStore( value, low8Ptr );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( value, GetConstant( 0, 8, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits8( value, 0x80 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );
}

void Recompiler::InstructionPull16( llvm::Value* register16Ptr )
{
	auto[ low8Ptr, high8Ptr ] = Recompiler::GetLowHighPtrFromPtr16( register16Ptr );

	auto low = Pull();
	m_IRBuilder.CreateStore( low, low8Ptr );
	auto high = Pull();
	m_IRBuilder.CreateStore( high, high8Ptr );

	auto newValue16 = m_IRBuilder.CreateLoad( register16Ptr );
	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( newValue16, GetConstant( 0, 16, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits16( newValue16, 0x8000 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );
}

void Recompiler::InsertBlockMoveInstructionBlock( llvm::Value* sourceBank32, llvm::Value* destinationBank32 )
{
	m_IRBuilder.CreateStore( m_IRBuilder.CreateTrunc( destinationBank32, llvm::Type::getInt8Ty( m_LLVMContext ) ), &m_registerDB );

	auto X16 = m_IRBuilder.CreateLoad( &m_registerX );
	auto X32 = m_IRBuilder.CreateZExt( X16, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto read8 = Read8( OrAllValues( m_IRBuilder.CreateShl( sourceBank32, 16 ), X32 ) );

	auto Y16 = m_IRBuilder.CreateLoad( &m_registerY );
	auto Y32 = m_IRBuilder.CreateZExt( Y16, llvm::Type::getInt32Ty( m_LLVMContext ) );
	Write8( OrAllValues( m_IRBuilder.CreateShl( destinationBank32, 16 ), Y32 ), read8 );
}

void Recompiler::InstructionBlockMove8( llvm::Value* sourceBank32, llvm::Value* destinationBank32, llvm::Value* adjust8, llvm::BasicBlock* blockMove, llvm::BasicBlock* endBlock )
{
	InsertBlockMoveInstructionBlock( sourceBank32, destinationBank32 );

	auto [xLow8Ptr, xHigh8Ptr] = GetLowHighPtrFromPtr16( &m_registerX );
	auto xLow8Value = m_IRBuilder.CreateLoad( xLow8Ptr );
	
	auto newX8 = m_IRBuilder.CreateAdd( xLow8Value, adjust8 );
	m_IRBuilder.CreateStore( newX8, xLow8Ptr );

	auto[ yLow8Ptr, yHigh8Ptr ] = GetLowHighPtrFromPtr16( &m_registerY );
	auto yLow8Value = m_IRBuilder.CreateLoad( yLow8Ptr );

	auto newY8 = m_IRBuilder.CreateAdd( yLow8Value, adjust8 );
	m_IRBuilder.CreateStore( newY8, yLow8Ptr );

	auto A16 = m_IRBuilder.CreateLoad( &m_registerA );
	m_IRBuilder.CreateStore( m_IRBuilder.CreateSub( A16, GetConstant( 1, 16, false ) ), &m_registerA );
	
	auto cond = m_IRBuilder.CreateICmpNE( A16, GetConstant( 0, 16, false ) );
	m_IRBuilder.CreateCondBr( cond, blockMove, endBlock );
}

void Recompiler::InstructionBlockMove16( llvm::Value* sourceBank32, llvm::Value* destinationBank32, llvm::Value* adjust16, llvm::BasicBlock* blockMove, llvm::BasicBlock* endBlock )
{
	InsertBlockMoveInstructionBlock( sourceBank32, destinationBank32 );

	auto X16 = m_IRBuilder.CreateLoad( &m_registerX );
	m_IRBuilder.CreateStore( m_IRBuilder.CreateAdd( X16, adjust16 ), &m_registerX );
	
	auto Y16 = m_IRBuilder.CreateLoad( &m_registerY );
	m_IRBuilder.CreateStore( m_IRBuilder.CreateAdd( Y16, adjust16 ), &m_registerY );

	auto A16 = m_IRBuilder.CreateLoad( &m_registerA );
	m_IRBuilder.CreateStore( m_IRBuilder.CreateSub( A16, GetConstant( 1, 16, false ) ), &m_registerA );

	auto cond = m_IRBuilder.CreateICmpNE( A16, GetConstant( 0, 16, false ) );
	m_IRBuilder.CreateCondBr( cond, blockMove, endBlock );
}

void Recompiler::PerformDirectIndexedModifyInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address16 )
{
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );

	SelectBlock( flagSetBlock );
	InstructionDirectIndexedModify8( op8, address16 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionDirectIndexedModify16( op16, address16 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformBankIndexedModifyInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* address16 )
{
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );

	SelectBlock( flagSetBlock );
	InstructionBankIndexedModify8( op8, address16 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionBankIndexedModify16( op16, address16 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::InstructionDirectModify8( Operation op, llvm::Value* address32 )
{
	auto read = ReadDirect( address32 );
	auto result = ( this->*op )( read );
	WriteDirect( address32, result );
}

void Recompiler::InstructionDirectModify16( Operation op, llvm::Value* address32 )
{
	auto readLow8 = ReadDirect( address32 );
	auto address32High = m_IRBuilder.CreateAdd( address32, GetConstant( 1, 32, false ) );
	auto readHigh8 = ReadDirect( address32High );

	auto read16 = CombineTo16( readLow8, readHigh8 );

	auto result16 = ( this->*op )( read16 );

	auto[ resultLow8, resultHigh8 ] = ConvertTo8( result16 );

	WriteDirect( address32High, resultHigh8 );
	WriteDirect( address32, resultLow8 );
}

void Recompiler::InstructionDirectIndexedModify8( Operation op, llvm::Value* address16 )
{
	auto X = m_IRBuilder.CreateLoad( &m_registerX );
	auto readDirectAddress32 = m_IRBuilder.CreateZExt( m_IRBuilder.CreateAdd( X, address16 ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto read = ReadDirect( readDirectAddress32 );
	auto result = ( this->*op )( read );
	WriteDirect( readDirectAddress32, result );
}

void Recompiler::InstructionDirectIndexedModify16( Operation op, llvm::Value* address16 )
{
	auto X = m_IRBuilder.CreateLoad( &m_registerX );
	auto readDirectAddressLow32 = m_IRBuilder.CreateZExt( m_IRBuilder.CreateAdd( X, address16 ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto readDirectAddressHigh32 = m_IRBuilder.CreateAdd( readDirectAddressLow32, GetConstant( 1, 32, false ) );
	auto readLow8 = ReadDirect( readDirectAddressLow32 );
	auto readHigh8 = ReadDirect( readDirectAddressHigh32 );

	auto read16 = CombineTo16( readLow8, readHigh8 );

	auto result16 = ( this->*op )( read16 );

	auto[ resultLow8, resultHigh8 ] = ConvertTo8( result16 );

	WriteDirect( readDirectAddressHigh32, resultHigh8 );
	WriteDirect( readDirectAddressLow32, resultLow8 );
}

llvm::Value* Recompiler::GetConstant( uint32_t value, uint32_t bitWidth, bool isSigned )
{
	return llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( bitWidth, static_cast<uint64_t>( value ), isSigned ) );
}

llvm::Value* Recompiler::TestBits8( llvm::Value* lhs, uint8_t rhs )
{
	auto test = m_IRBuilder.CreateAnd( lhs, GetConstant( rhs, 8, false ) );
	return m_IRBuilder.CreateICmpNE( test, GetConstant( 0, 8, false ) );
}

llvm::Value* Recompiler::TestBits16( llvm::Value* lhs, uint16_t rhs )
{
	auto test = m_IRBuilder.CreateAnd( lhs, GetConstant( rhs, 16, false ) );
	return m_IRBuilder.CreateICmpNE( test, GetConstant( 0, 16, false ) );
}

llvm::Value* Recompiler::TestBits32( llvm::Value* lhs, uint32_t rhs )
{
	auto test = m_IRBuilder.CreateAnd( lhs, GetConstant( rhs, 32, false ) );
	return m_IRBuilder.CreateICmpNE( test, GetConstant( 0, 32, false ) );
}

llvm::Value* Recompiler::BIT8( llvm::Value* value )
{
	auto A = m_IRBuilder.CreateLoad( &m_registerA );
	auto A8 = m_IRBuilder.CreateTrunc( A, llvm::Type::getInt8Ty( m_LLVMContext ) );
	auto tempResult = m_IRBuilder.CreateAnd( value, A8 );
	
	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( tempResult, GetConstant( 0, 8, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto overflowFlagResult = TestBits8( value, 0x40 );
	m_IRBuilder.CreateStore( overflowFlagResult, &m_OverflowFlag );

	auto negativeFlagResult = TestBits8( value, 0x80 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return value;
}

llvm::Value* Recompiler::BIT16( llvm::Value* value )
{
	auto A = m_IRBuilder.CreateLoad( &m_registerA );
	auto tempResult = m_IRBuilder.CreateAnd( value, A );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( tempResult, GetConstant( 0, 16, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto overflowFlagResult = TestBits16( value, 0x4000 );
	m_IRBuilder.CreateStore( overflowFlagResult, &m_OverflowFlag );

	auto negativeFlagResult = TestBits16( value, 0x8000 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );
	
	return value;
}

llvm::Value* Recompiler::DEC8( llvm::Value* value )
{
	auto result = m_IRBuilder.CreateSub( value, GetConstant( 1, 8, false ) );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( result, GetConstant( 0, 8, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits8( result, 0x80 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return result;
}

llvm::Value* Recompiler::DEC16( llvm::Value* value )
{
	auto result = m_IRBuilder.CreateSub( value, GetConstant( 1, 16, false ) );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( result, GetConstant( 0, 16, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits16( result, 0x8000 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return result;
}

llvm::Value* Recompiler::INC8( llvm::Value* value )
{
	auto result = m_IRBuilder.CreateAdd( value, GetConstant( 1, 8, false ) );
	
	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( result, GetConstant( 0, 8, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits8( result, 0x80 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return result;
}

llvm::Value* Recompiler::INC16( llvm::Value* value )
{
	auto result = m_IRBuilder.CreateAdd( value, GetConstant( 1, 16, false ) );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( result, GetConstant( 0, 16, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits16( result, 0x8000 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return result;
}

llvm::Value* Recompiler::LSR8( llvm::Value* value )
{
	auto carryFlagResult = TestBits8( value, 0x01 );
	m_IRBuilder.CreateStore( carryFlagResult, &m_CarryFlag );

	auto result = m_IRBuilder.CreateLShr( value, 1 );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( result, GetConstant( 0, 8, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits8( result, 0x80 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return result;
}

llvm::Value* Recompiler::LSR16( llvm::Value* value )
{
	auto carryFlagResult = TestBits16( value, 0x01 );
	m_IRBuilder.CreateStore( carryFlagResult, &m_CarryFlag );

	auto result = m_IRBuilder.CreateLShr( value, 1 );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( result, GetConstant( 0, 16, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits16( result, 0x8000 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return result;
}

llvm::Value* Recompiler::ROR8( llvm::Value* value )
{
	auto carry = m_IRBuilder.CreateLoad( &m_CarryFlag );
	auto carry8 = m_IRBuilder.CreateZExt( carry, llvm::Type::getInt8Ty( m_LLVMContext ) );
	
	auto carryFlagResult = TestBits8( value, 0x01 );
	m_IRBuilder.CreateStore( carryFlagResult, &m_CarryFlag );

	auto result = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( carry8, 7 ), m_IRBuilder.CreateLShr( value, 1 ) );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( result, GetConstant( 0, 8, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits8( result, 0x80 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return result;
}

llvm::Value* Recompiler::ROR16( llvm::Value* value )
{
	auto carry = m_IRBuilder.CreateLoad( &m_CarryFlag );
	auto carry16 = m_IRBuilder.CreateZExt( carry, llvm::Type::getInt16Ty( m_LLVMContext ) );

	auto carryFlagResult = TestBits16( value, 0x01 );
	m_IRBuilder.CreateStore( carryFlagResult, &m_CarryFlag );

	auto result = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( carry16, 15 ), m_IRBuilder.CreateLShr( value, 1 ) );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( result, GetConstant( 0, 16, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits16( result, 0x8000 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return result;
}

llvm::Value* Recompiler::ROL8( llvm::Value* value )
{
	auto carry = m_IRBuilder.CreateLoad( &m_CarryFlag );
	auto carry8 = m_IRBuilder.CreateZExt( carry, llvm::Type::getInt8Ty( m_LLVMContext ) );

	auto carryFlagResult = TestBits8( value, 0x80 );
	m_IRBuilder.CreateStore( carryFlagResult, &m_CarryFlag );
	
	auto result = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( value, 1 ), carry8 );
	
	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( result, GetConstant( 0, 8, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );
	
	auto negativeFlagResult = TestBits8( result, 0x80 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return result;
}

llvm::Value* Recompiler::ROL16( llvm::Value* value )
{
	auto carry = m_IRBuilder.CreateLoad( &m_CarryFlag );
	auto carry16 = m_IRBuilder.CreateZExt( carry, llvm::Type::getInt16Ty( m_LLVMContext ) );

	auto carryFlagResult = TestBits16( value, 0x8000 );
	m_IRBuilder.CreateStore( carryFlagResult, &m_CarryFlag );

	auto result = m_IRBuilder.CreateOr( m_IRBuilder.CreateShl( value, 1 ), carry16 );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( result, GetConstant( 0, 16, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits16( result, 0x8000 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return result;
}

llvm::Value* Recompiler::ORA8( llvm::Value* value )
{
	auto A = m_IRBuilder.CreateLoad( &m_registerA );
	auto A8 = m_IRBuilder.CreateTrunc( A, llvm::Type::getInt8Ty( m_LLVMContext ) );
	auto result = m_IRBuilder.CreateOr( A8, value );
	m_IRBuilder.CreateStore( result, m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ) ) );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( result, GetConstant( 0, 8, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );
	
	auto negativeFlagResult = TestBits8( result, 0x80 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return result;
}

llvm::Value* Recompiler::ORA16( llvm::Value* value )
{
	auto A = m_IRBuilder.CreateLoad( &m_registerA );
	auto result = m_IRBuilder.CreateOr( A, value );
	m_IRBuilder.CreateStore( result, &m_registerA );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( result, GetConstant( 0, 16, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits16( result, 0x8000 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return result;
}

llvm::Value* Recompiler::AND8( llvm::Value* value )
{
	auto A = m_IRBuilder.CreateLoad( &m_registerA );
	auto A8 = m_IRBuilder.CreateTrunc( A, llvm::Type::getInt8Ty( m_LLVMContext ) );
	auto result = m_IRBuilder.CreateAnd( A8, value );
	m_IRBuilder.CreateStore( result, m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ) ) );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( result, GetConstant( 0, 8, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits8( result, 0x80 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return result;
}

llvm::Value* Recompiler::AND16( llvm::Value* value )
{
	auto A = m_IRBuilder.CreateLoad( &m_registerA );
	auto result = m_IRBuilder.CreateAnd( A, value );
	m_IRBuilder.CreateStore( result, &m_registerA );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( result, GetConstant( 0, 16, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits16( result, 0x8000 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return result;
}

llvm::Value* Recompiler::EOR8( llvm::Value* value )
{
	auto A = m_IRBuilder.CreateLoad( &m_registerA );
	auto A8 = m_IRBuilder.CreateTrunc( A, llvm::Type::getInt8Ty( m_LLVMContext ) );
	auto result = m_IRBuilder.CreateXor( A8, value );
	m_IRBuilder.CreateStore( result, m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ) ) );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( result, GetConstant( 0, 8, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );
	
	auto negativeFlagResult = TestBits8( result, 0x80 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return result;
}

llvm::Value* Recompiler::EOR16( llvm::Value* value )
{
	auto A = m_IRBuilder.CreateLoad( &m_registerA );
	auto result = m_IRBuilder.CreateXor( A, value );
	m_IRBuilder.CreateStore( result, &m_registerA );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( result, GetConstant( 0, 16, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits16( result, 0x8000 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return result;
}

llvm::Value* Recompiler::LDY8( llvm::Value* value )
{
	m_IRBuilder.CreateStore( value, m_IRBuilder.CreateBitCast( &m_registerY, llvm::Type::getInt8PtrTy( m_LLVMContext ) ) );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( value, GetConstant( 0, 8, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits8( value, 0x80 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return value;
}

llvm::Value* Recompiler::LDY16( llvm::Value* value )
{
	m_IRBuilder.CreateStore( value, &m_registerY );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( value, GetConstant( 0, 16, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits16( value, 0x8000 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return value;
}

llvm::Value* Recompiler::LDX8( llvm::Value* value )
{
	m_IRBuilder.CreateStore( value, m_IRBuilder.CreateBitCast( &m_registerX, llvm::Type::getInt8PtrTy( m_LLVMContext ) ) );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( value, GetConstant( 0, 8, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits8( value, 0x80 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return value;
}

llvm::Value* Recompiler::LDX16( llvm::Value* value )
{
	m_IRBuilder.CreateStore( value, &m_registerX );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( value, GetConstant( 0, 16, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits16( value, 0x8000 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return value;
}

llvm::Value* Recompiler::LDA8( llvm::Value* value )
{
	m_IRBuilder.CreateStore( value, m_IRBuilder.CreateBitCast( &m_registerA, llvm::Type::getInt8PtrTy( m_LLVMContext ) ) );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( value, GetConstant( 0, 8, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits8( value, 0x80 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return value;
}

llvm::Value* Recompiler::LDA16( llvm::Value* value )
{
	m_IRBuilder.CreateStore( value, &m_registerA );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( value, GetConstant( 0, 16, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits16( value, 0x8000 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return value;
}

llvm::Value* Recompiler::CPY8( llvm::Value* value )
{
	auto Y8 = m_IRBuilder.CreateTrunc( m_IRBuilder.CreateLoad( &m_registerY ), llvm::Type::getInt8Ty( m_LLVMContext ) );
	auto Y32 = m_IRBuilder.CreateZExt( Y8, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto value32 = m_IRBuilder.CreateZExt( value, llvm::Type::getInt32Ty( m_LLVMContext ) );

	auto result32 = m_IRBuilder.CreateSub( Y32, value32 );

	auto carryFlagResult = m_IRBuilder.CreateICmpSGE( result32, GetConstant( 0, 32, false ) );
	m_IRBuilder.CreateStore( carryFlagResult, &m_CarryFlag );

	auto result = m_IRBuilder.CreateTrunc( result32, llvm::Type::getInt8Ty( m_LLVMContext ) );
	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( result, GetConstant( 0, 8, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits8( result, 0x80 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return result;
}

llvm::Value* Recompiler::CPY16( llvm::Value* value )
{
	auto Y32 = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerY ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto value32 = m_IRBuilder.CreateZExt( value, llvm::Type::getInt32Ty( m_LLVMContext ) );

	auto result32 = m_IRBuilder.CreateSub( Y32, value32 );

	auto carryFlagResult = m_IRBuilder.CreateICmpSGE( result32, GetConstant( 0, 32, false ) );
	m_IRBuilder.CreateStore( carryFlagResult, &m_CarryFlag );

	auto result = m_IRBuilder.CreateTrunc( result32, llvm::Type::getInt16Ty( m_LLVMContext ) );
	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( result, GetConstant( 0, 16, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits16( result, 0x8000 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return result;
}

llvm::Value* Recompiler::CPX8( llvm::Value* value )
{
	auto X8 = m_IRBuilder.CreateTrunc( m_IRBuilder.CreateLoad( &m_registerX ), llvm::Type::getInt8Ty( m_LLVMContext ) );
	auto X32 = m_IRBuilder.CreateZExt( X8, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto value32 = m_IRBuilder.CreateZExt( value, llvm::Type::getInt32Ty( m_LLVMContext ) );

	auto result32 = m_IRBuilder.CreateSub( X32, value32 );

	auto carryFlagResult = m_IRBuilder.CreateICmpSGE( result32, GetConstant( 0, 32, false ) );
	m_IRBuilder.CreateStore( carryFlagResult, &m_CarryFlag );

	auto result = m_IRBuilder.CreateTrunc( result32, llvm::Type::getInt8Ty( m_LLVMContext ) );
	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( result, GetConstant( 0, 8, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits8( result, 0x80 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return result;
}

llvm::Value* Recompiler::CPX16( llvm::Value* value )
{
	auto X32 = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerX ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto value32 = m_IRBuilder.CreateZExt( value, llvm::Type::getInt32Ty( m_LLVMContext ) );

	auto result32 = m_IRBuilder.CreateSub( X32, value32 );

	auto carryFlagResult = m_IRBuilder.CreateICmpSGE( result32, GetConstant( 0, 32, false ) );
	m_IRBuilder.CreateStore( carryFlagResult, &m_CarryFlag );

	auto result = m_IRBuilder.CreateTrunc( result32, llvm::Type::getInt16Ty( m_LLVMContext ) );
	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( result, GetConstant( 0, 16, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits16( result, 0x8000 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return result;
}

llvm::Value* Recompiler::CMP8( llvm::Value* value )
{
	auto A8 = m_IRBuilder.CreateTrunc( m_IRBuilder.CreateLoad( &m_registerA ), llvm::Type::getInt8Ty( m_LLVMContext ) );
	auto A32 = m_IRBuilder.CreateZExt( A8, llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto value32 = m_IRBuilder.CreateZExt( value, llvm::Type::getInt32Ty( m_LLVMContext ) );

	auto result32 = m_IRBuilder.CreateSub( A32, value32 );

	auto carryFlagResult = m_IRBuilder.CreateICmpSGE( result32, GetConstant( 0, 32, false ) );
	m_IRBuilder.CreateStore( carryFlagResult, &m_CarryFlag );

	auto result = m_IRBuilder.CreateTrunc( result32, llvm::Type::getInt8Ty( m_LLVMContext ) );
	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( result, GetConstant( 0, 8, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits8( result, 0x80 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return result;
}

llvm::Value* Recompiler::CMP16( llvm::Value* value )
{
	auto A32 = m_IRBuilder.CreateZExt( m_IRBuilder.CreateLoad( &m_registerA ), llvm::Type::getInt32Ty( m_LLVMContext ) );
	auto value32 = m_IRBuilder.CreateZExt( value, llvm::Type::getInt32Ty( m_LLVMContext ) );

	auto result32 = m_IRBuilder.CreateSub( A32, value32 );

	auto carryFlagResult = m_IRBuilder.CreateICmpSGE( result32, GetConstant( 0, 32, false ) );
	m_IRBuilder.CreateStore( carryFlagResult, &m_CarryFlag );

	auto result = m_IRBuilder.CreateTrunc( result32, llvm::Type::getInt16Ty( m_LLVMContext ) );
	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( result, GetConstant( 0, 16, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits16( result, 0x8000 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );

	return result;
}

llvm::Value* Recompiler::ADC8( llvm::Value* value )
{
	return m_IRBuilder.CreateCall( m_ADC8Function, { value } );
}

llvm::Value* Recompiler::ADC16( llvm::Value* value )
{
	return m_IRBuilder.CreateCall( m_ADC16Function, { value } );
}

llvm::Value* Recompiler::SBC8( llvm::Value* value )
{
	return m_IRBuilder.CreateCall( m_SBC8Function, { value } );
}

llvm::Value* Recompiler::SBC16( llvm::Value* value )
{
	return m_IRBuilder.CreateCall( m_SBC16Function, { value } );
}

llvm::Value* Recompiler::ASL8( llvm::Value* value )
{
	auto carryFlagResult = TestBits8( value, 0x80 );
	m_IRBuilder.CreateStore( carryFlagResult, &m_CarryFlag );
	
	auto result = m_IRBuilder.CreateShl( value, 1 );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( result, GetConstant( 0, 8, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits8( result, 0x80 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );
	
	return result;
}

llvm::Value* Recompiler::ASL16( llvm::Value* value )
{
	auto carryFlagResult = TestBits16( value, 0x8000 );
	m_IRBuilder.CreateStore( carryFlagResult, &m_CarryFlag );

	auto result = m_IRBuilder.CreateShl( value, 1 );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( result, GetConstant( 0, 16, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto negativeFlagResult = TestBits16( result, 0x8000 );
	m_IRBuilder.CreateStore( negativeFlagResult, &m_NegativeFlag );
	
	return result;
}

llvm::Value* Recompiler::TRB8( llvm::Value* value )
{
	auto A = m_IRBuilder.CreateLoad( &m_registerA );
	auto A8 = m_IRBuilder.CreateTrunc( A, llvm::Type::getInt8Ty( m_LLVMContext ) );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( m_IRBuilder.CreateAnd( A8, value ), GetConstant( 0, 8, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto complementA8 = m_IRBuilder.CreateXor( A8, GetConstant( 0xff, 8, false ) );
	auto result = m_IRBuilder.CreateAnd( value, complementA8 );
	return result;
}

llvm::Value* Recompiler::TRB16( llvm::Value* value )
{
	auto A = m_IRBuilder.CreateLoad( &m_registerA );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( m_IRBuilder.CreateAnd( A, value ), GetConstant( 0, 16, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto complementA = m_IRBuilder.CreateXor( A, GetConstant( 0xffff, 16, false ) );
	auto result = m_IRBuilder.CreateAnd( value, complementA );
	return result;
}

llvm::Value* Recompiler::TSB8( llvm::Value* value )
{
	auto A = m_IRBuilder.CreateLoad( &m_registerA );
	auto A8 = m_IRBuilder.CreateTrunc( A, llvm::Type::getInt8Ty( m_LLVMContext ) );
	
	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( m_IRBuilder.CreateAnd( A8, value ), GetConstant( 0, 8, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto result = m_IRBuilder.CreateOr( value, A8 );
	return result;
}

llvm::Value* Recompiler::TSB16( llvm::Value* value )
{
	auto A = m_IRBuilder.CreateLoad( &m_registerA );

	auto zeroFlagResult = m_IRBuilder.CreateICmpEQ( m_IRBuilder.CreateAnd( A, value ), GetConstant( 0, 16, false ) );
	m_IRBuilder.CreateStore( zeroFlagResult, &m_ZeroFlag );

	auto result = m_IRBuilder.CreateOr( value, A );
	return result;
}

void Recompiler::PerformImmediateReadInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* operand16 )
{
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );

	SelectBlock( flagSetBlock );
	auto operand8 = m_IRBuilder.CreateTrunc( operand16, llvm::Type::getInt8Ty( m_LLVMContext ) );
	InstructionImmediateRead8( op8, operand8 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionImmediateRead16( op16, operand16 );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::PerformImpliedModifyInstruction( Operation op8, Operation op16, RegisterModeFlag modeFlag, llvm::Value* ptr )
{
	auto[ flagSetBlock, flagNotSetBlock, endBlock ] = CreateRegisterFlagTestBlock( modeFlag == RegisterModeFlag::REGISTER_MODE_FLAG_M ? &m_AccumulatorFlag : &m_IndexRegisterFlag );

	SelectBlock( flagSetBlock );
	InstructionImpliedModify8( op8, ptr );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( flagNotSetBlock );
	InstructionImpliedModify16( op16, ptr );
	m_IRBuilder.CreateBr( endBlock );

	SelectBlock( endBlock );
}

void Recompiler::GenerateCodeForInstruction( const Instruction& instruction, const std::string& functionName )
{
	PerformUpdateInstructionOutput( instruction.GetOffset(), instruction.GetPC(), instruction.GetInstructionString() );
	PerformRomCycle( llvm::ConstantInt::get( m_LLVMContext, llvm::APInt( 32, static_cast<uint64_t>( 0 ), false ) ) );
	switch ( instruction.GetOpcode() )
	{
		case 0x00:
			// BRK - Nothing to do.
			break;
		case 0x01:
			PerformIndexedIndirectReadInstruction( &Recompiler::ORA8, &Recompiler::ORA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x02:
			// COP - Nothing to do.
			break;
		case 0x03:
			PerformStackReadInstruction( &Recompiler::ORA8, &Recompiler::ORA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x04:
			PerformDirectModifyInstruction( &Recompiler::TSB8, &Recompiler::TSB16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x05:
			PerformDirectReadInstruction( &Recompiler::ORA8, &Recompiler::ORA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x06:
			PerformDirectModifyInstruction( &Recompiler::ASL8, &Recompiler::ASL16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x07:
			PerformIndirectLongReadInstruction( &Recompiler::ORA8, &Recompiler::ORA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), GetConstant( 0, 16, false ) );
			break;
		case 0x08:
			PerformPush8Instruction( GetProcessorStatusRegisterValueFromFlags() );
			break;
		case 0x09:
			PerformImmediateReadInstruction( &Recompiler::ORA8, &Recompiler::ORA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ) );
			break;
		case 0x0a:
			PerformImpliedModifyInstruction( &Recompiler::ASL8, &Recompiler::ASL16, RegisterModeFlag::REGISTER_MODE_FLAG_M, &m_registerA );
			break;
		case 0x0b:
			PerformPushDInstruction();
			break;
		case 0x0c:
			PerformBankModifyInstruction( &Recompiler::TSB8, &Recompiler::TSB16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x0d:
			PerformBankReadInstruction( &Recompiler::ORA8, &Recompiler::ORA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x0e:
			PerformBankModifyInstruction( &Recompiler::ASL8, &Recompiler::ASL16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x0f:
			PerformLongReadInstruction( &Recompiler::ASL8, &Recompiler::ASL16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), GetConstant( 0, 16, false ) );
			break;
		case 0x10:
			{
			auto NF = m_IRBuilder.CreateLoad( &m_NegativeFlag );
			auto NFCond = m_IRBuilder.CreateICmpEQ( NF, GetConstant( 0, 1, false ) );
			PerformBranchInstruction( NFCond, instruction.GetJumpLabelName(), functionName );
			}
			break;
		case 0x11:
			PerformIndirectIndexedReadInstruction( &Recompiler::ORA8, &Recompiler::ORA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x12:
			PerformIndirectReadInstruction( &Recompiler::ORA8, &Recompiler::ORA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x13:
			PerformIndirectStackReadInstruction( &Recompiler::ORA8, &Recompiler::ORA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x14:
			PerformDirectModifyInstruction( &Recompiler::TRB8, &Recompiler::TRB16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x15:
			PerformDirectReadInstruction( &Recompiler::ORA8, &Recompiler::ORA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0x16:
			PerformBankIndexedModifyInstruction( &Recompiler::ASL8, &Recompiler::ASL16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ) );
			break;
		case 0x17:
			PerformIndirectLongReadInstruction( &Recompiler::ORA8, &Recompiler::ORA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerY ) );
			break;
		case 0x18:
			PerformClearFlagInstruction( &m_CarryFlag );
			break;
		case 0x19:
			PerformBankReadInstruction( &Recompiler::ORA8, &Recompiler::ORA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ), m_IRBuilder.CreateLoad( &m_registerY ) );
			break;
		case 0x1a:
			PerformImpliedModifyInstruction( &Recompiler::INC8, &Recompiler::INC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, &m_registerA );
			break;
		case 0x1b:
			PerformTransferCSInstruction();
			break;
		case 0x1c:
			PerformBankModifyInstruction( &Recompiler::TRB8, &Recompiler::TRB16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x1d:
			PerformBankReadInstruction( &Recompiler::ORA8, &Recompiler::ORA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0x1e:
			PerformBankIndexedModifyInstruction( &Recompiler::ASL8, &Recompiler::ASL16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ) );
			break;
		case 0x1f:
			PerformLongReadInstruction( &Recompiler::ORA8, &Recompiler::ORA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0x20:
			PerformCallShortInstruction( instruction.GetOffset() );
			break;
		case 0x21:
			PerformIndexedIndirectReadInstruction( &Recompiler::AND8, &Recompiler::AND16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x22:
			PerformCallLongInstruction( instruction.GetOffset() );
			break;
		case 0x23:
			PerformStackReadInstruction( &Recompiler::AND8, &Recompiler::AND16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x24:
			PerformDirectReadInstruction( &Recompiler::BIT8, &Recompiler::BIT16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x25:
			PerformDirectReadInstruction( &Recompiler::AND8, &Recompiler::AND16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x26:
			PerformDirectModifyInstruction( &Recompiler::ROL8, &Recompiler::ROL16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x27:
			PerformIndirectLongReadInstruction( &Recompiler::AND8, &Recompiler::AND16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), GetConstant( 0, 16, false ) );
			break;
		case 0x28:
			PerformPullPInstruction();
			break;
		case 0x29:
			PerformImmediateReadInstruction( &Recompiler::AND8, &Recompiler::AND16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ) );
			break;
		case 0x2a:
			PerformImpliedModifyInstruction( &Recompiler::ROL8, &Recompiler::ROL16, RegisterModeFlag::REGISTER_MODE_FLAG_M, &m_registerA );
			break;
		case 0x2b:
			PerformPullDInstruction();
			break;
		case 0x2c:
			PerformBankReadInstruction( &Recompiler::BIT8, &Recompiler::BIT16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x2d:
			PerformBankReadInstruction( &Recompiler::AND8, &Recompiler::AND16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x2e:
			PerformBankModifyInstruction( &Recompiler::ROL8, &Recompiler::ROL16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x2f:
			PerformLongReadInstruction( &Recompiler::AND8, &Recompiler::AND16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), GetConstant( 0, 16, false ) );
			break;
		case 0x30:
			{
			auto NF = m_IRBuilder.CreateLoad( &m_NegativeFlag );
			auto NFCond = m_IRBuilder.CreateICmpEQ( NF, GetConstant( 1, 1, false ) );
			PerformBranchInstruction( NFCond, instruction.GetJumpLabelName(), functionName );
			}
			break;
		case 0x31:
			PerformIndirectIndexedReadInstruction( &Recompiler::AND8, &Recompiler::AND16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x32:
			PerformIndirectReadInstruction( &Recompiler::AND8, &Recompiler::AND16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x33:
			PerformIndirectStackReadInstruction( &Recompiler::AND8, &Recompiler::AND16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x34:
			PerformDirectReadInstruction( &Recompiler::BIT8, &Recompiler::BIT16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0x35:
			PerformDirectReadInstruction( &Recompiler::AND8, &Recompiler::AND16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0x36:
			PerformBankIndexedModifyInstruction( &Recompiler::ROL8, &Recompiler::ROL16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ) );
			break;
		case 0x37:
			PerformIndirectLongReadInstruction( &Recompiler::AND8, &Recompiler::AND16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerY ) );
			break;
		case 0x38:
			PerformSetFlagInstruction( &m_CarryFlag );
			break;
		case 0x39:
			PerformBankReadInstruction( &Recompiler::AND8, &Recompiler::AND16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ), m_IRBuilder.CreateLoad( &m_registerY ) );
			break;
		case 0x3a:
			PerformImpliedModifyInstruction( &Recompiler::DEC8, &Recompiler::DEC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, &m_registerA );
			break;
		case 0x3b:
			PerformTransfer16Instruction( &m_registerSP, &m_registerA );
			break;
		case 0x3c:
			PerformBankReadInstruction( &Recompiler::BIT8, &Recompiler::BIT16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0x3d:
			PerformBankReadInstruction( &Recompiler::AND8, &Recompiler::AND16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0x3e:
			PerformBankIndexedModifyInstruction( &Recompiler::ROL8, &Recompiler::ROL16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ) );
			break;
		case 0x3f:
			PerformLongReadInstruction( &Recompiler::AND8, &Recompiler::AND16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0x40:
			PerformReturnInterruptInstruction();
			break;
		case 0x41:
			PerformIndexedIndirectReadInstruction( &Recompiler::EOR8, &Recompiler::EOR16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x42:
			// WDM - Nothing to do.
			break;
		case 0x43:
			PerformStackReadInstruction( &Recompiler::EOR8, &Recompiler::EOR16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x44:
			PerformBlockMoveInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_X, GetConstant( instruction.GetOperand(), 32, false ), GetConstant( -1, 16, true ) );
			break;
		case 0x45:
			PerformDirectReadInstruction( &Recompiler::EOR8, &Recompiler::EOR16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x46:
			PerformDirectModifyInstruction( &Recompiler::LSR8, &Recompiler::LSR16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x47:
			PerformIndirectLongReadInstruction( &Recompiler::EOR8, &Recompiler::EOR16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), GetConstant( 0, 16, false ) );
			break;
		case 0x48:
			PerformPushInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_M, m_IRBuilder.CreateLoad( &m_registerA ) );
			break;
		case 0x49:
			PerformImmediateReadInstruction( &Recompiler::EOR8, &Recompiler::EOR16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ) );
			break;
		case 0x4a:
			PerformImpliedModifyInstruction( &Recompiler::LSR8, &Recompiler::LSR16, RegisterModeFlag::REGISTER_MODE_FLAG_M, &m_registerA );
			break;
		case 0x4b:
			PerformPush8Instruction( m_IRBuilder.CreateLoad( &m_registerPB ) );
			break;
		case 0x4c:
			PerformJumpInstruction( instruction.GetJumpLabelName(), functionName );
			break;
		case 0x4d:
			PerformBankReadInstruction( &Recompiler::EOR8, &Recompiler::EOR16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x4e:
			PerformBankModifyInstruction( &Recompiler::LSR8, &Recompiler::LSR16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x4f:
			PerformLongReadInstruction( &Recompiler::EOR8, &Recompiler::EOR16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), GetConstant( 0, 16, false ) );
			break;
		case 0x50:
			{
			auto VF = m_IRBuilder.CreateLoad( &m_OverflowFlag );
			auto VFCond = m_IRBuilder.CreateICmpEQ( VF, GetConstant( 0, 1, false ) );
			PerformBranchInstruction( VFCond, instruction.GetJumpLabelName(), functionName );
			}
			break;
		case 0x51:
			PerformIndirectIndexedReadInstruction( &Recompiler::EOR8, &Recompiler::EOR16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x52:
			PerformIndirectReadInstruction( &Recompiler::EOR8, &Recompiler::EOR16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x53:
			PerformIndirectStackReadInstruction( &Recompiler::EOR8, &Recompiler::EOR16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x54:
			PerformBlockMoveInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_X, GetConstant( instruction.GetOperand(), 32, false ), GetConstant( 1, 16, true ) );
			break;
		case 0x55:
			PerformDirectReadInstruction( &Recompiler::EOR8, &Recompiler::EOR16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0x56:
			PerformBankIndexedModifyInstruction( &Recompiler::LSR8, &Recompiler::LSR16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ) );
			break;
		case 0x57:
			PerformIndirectLongReadInstruction( &Recompiler::EOR8, &Recompiler::EOR16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerY ) );
			break;
		case 0x58:
			PerformClearFlagInstruction( &m_InterruptFlag );
			break;
		case 0x59:
			PerformBankReadInstruction( &Recompiler::EOR8, &Recompiler::EOR16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ), m_IRBuilder.CreateLoad( &m_registerY ) );
			break;
		case 0x5a:
			PerformPushInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_X, m_IRBuilder.CreateLoad( &m_registerY ) );
			break;
		case 0x5b:
			PerformTransfer16Instruction( &m_registerA, &m_registerDP );
			break;
		case 0x5c:
			PerformJumpInstruction( instruction.GetJumpLabelName(), functionName );
			break;
		case 0x5d:
			PerformBankReadInstruction( &Recompiler::EOR8, &Recompiler::EOR16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0x5e:
			PerformBankIndexedModifyInstruction( &Recompiler::LSR8, &Recompiler::LSR16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ) );
			break;
		case 0x5f:
			PerformLongReadInstruction( &Recompiler::EOR8, &Recompiler::EOR16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0x60:
			PerformReturnShortInstruction();
			break;
		case 0x61:
			PerformIndexedIndirectReadInstruction( &Recompiler::ADC8, &Recompiler::ADC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x62:
			PerformPushEffectiveRelativeAddressInstruction( GetConstant( instruction.GetOperand(), 16, true ) );
			break;
		case 0x63:
			PerformStackReadInstruction( &Recompiler::ADC8, &Recompiler::ADC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x64:
			PerformDirectWriteInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), GetConstant( 0, 16, false ) );
			break;
		case 0x65:
			PerformDirectReadInstruction( &Recompiler::ADC8, &Recompiler::ADC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x66:
			PerformDirectModifyInstruction( &Recompiler::ROR8, &Recompiler::ROR16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x67:
			PerformIndirectLongReadInstruction( &Recompiler::ADC8, &Recompiler::ADC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), GetConstant( 0, 16, false ) );
			break;
		case 0x68:
			PerformPullInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_M, &m_registerA );
			break;
		case 0x69:
			PerformImmediateReadInstruction( &Recompiler::ADC8, &Recompiler::ADC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ) );
			break;
		case 0x6a:
			PerformImpliedModifyInstruction( &Recompiler::ROR8, &Recompiler::ROR16, RegisterModeFlag::REGISTER_MODE_FLAG_M, &m_registerA );
			break;
		case 0x6b:
			PerformReturnLongInstruction();
			break;
		case 0x6c:
			PerformJumpIndirectInstruction( instruction.GetOffset(), GetConstant( instruction.GetOperand(), 16, false ), functionName );
			break;
		case 0x6d:
			PerformBankReadInstruction( &Recompiler::ADC8, &Recompiler::ADC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x6e:
			PerformBankModifyInstruction( &Recompiler::ROR8, &Recompiler::ROR16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x6f:
			PerformLongReadInstruction( &Recompiler::ADC8, &Recompiler::ADC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), GetConstant( 0, 16, false ) );
			break;
		case 0x70:
			{
			auto VF = m_IRBuilder.CreateLoad( &m_OverflowFlag );
			auto VFCond = m_IRBuilder.CreateICmpEQ( VF, GetConstant( 1, 1, false ) );
			PerformBranchInstruction( VFCond, instruction.GetJumpLabelName(), functionName );
			}
			break;
		case 0x71:
			PerformIndirectIndexedReadInstruction( &Recompiler::ADC8, &Recompiler::ADC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x72:
			PerformIndirectReadInstruction( &Recompiler::ADC8, &Recompiler::ADC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x73:
			PerformIndirectStackReadInstruction( &Recompiler::ADC8, &Recompiler::ADC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x74:
			PerformDirectWriteInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerX ), GetConstant( 0, 16, false ) );
			break;
		case 0x75:
			PerformDirectReadInstruction( &Recompiler::ADC8, &Recompiler::ADC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0x76:
			PerformBankIndexedModifyInstruction( &Recompiler::ROR8, &Recompiler::ROR16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ) );
			break;
		case 0x77:
			PerformIndirectLongReadInstruction( &Recompiler::ADC8, &Recompiler::ADC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerY ) );
			break;
		case 0x78:
			PerformSetFlagInstruction( &m_InterruptFlag );
			break;
		case 0x79:
			PerformBankReadInstruction( &Recompiler::ADC8, &Recompiler::ADC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ), m_IRBuilder.CreateLoad( &m_registerY ) );
			break;
		case 0x7a:
			PerformPullInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_X, &m_registerY );
			break;
		case 0x7b:
			PerformTransfer16Instruction( &m_registerDP, &m_registerA );
			break;
		case 0x7c:
			PerformJumpIndexedIndirectInstruction( instruction.GetOffset(), GetConstant( instruction.GetOperand(), 16, false ), functionName );
			break;
		case 0x7d:
			PerformBankReadInstruction( &Recompiler::ADC8, &Recompiler::ADC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0x7e:
			PerformBankIndexedModifyInstruction( &Recompiler::ROR8, &Recompiler::ROR16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ) );
			break;
		case 0x7f:
			PerformLongReadInstruction( &Recompiler::ADC8, &Recompiler::ADC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0x80:
			PerformBranchInstruction( GetConstant( 1, 1, false ), instruction.GetJumpLabelName(), functionName );
			break;
		case 0x81:
			PerformIndexedIndirectWriteInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x82:
			PerformBranchInstruction( GetConstant( 1, 1, false ), instruction.GetJumpLabelName(), functionName );
			break;
		case 0x83:
			PerformStackWriteInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x84:
			PerformDirectWriteInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_X, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerY ) );
			break;
		case 0x85:
			PerformDirectWriteInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerA ) );
			break;
		case 0x86:
			PerformDirectWriteInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_X, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0x87:
			PerformIndirectLongWriteInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), GetConstant( 0, 16, false ) );
			break;
		case 0x88:
			PerformImpliedModifyInstruction( &Recompiler::DEC8, &Recompiler::DEC16, RegisterModeFlag::REGISTER_MODE_FLAG_X, &m_registerY );
			break;
		case 0x89:
			PerformBitImmediateInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ) );
			break;
		case 0x8a:
			PerformTransferInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_M, &m_registerX, &m_registerA );
			break;
		case 0x8b:
			PerformPush8Instruction( m_IRBuilder.CreateLoad( &m_registerDB ) );
			break;
		case 0x8c:
			PerformBankWriteInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_X, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerY ) );
			break;
		case 0x8d:
			PerformBankWriteInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerA ) );
			break;
		case 0x8e:
			PerformBankWriteInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_X, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0x8f:
			PerformLongWriteInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), GetConstant( 0, 16, false ) );
			break;
		case 0x90:
			{
			auto CF = m_IRBuilder.CreateLoad( &m_CarryFlag );
			auto CFCond = m_IRBuilder.CreateICmpEQ( CF, GetConstant( 0, 1, false ) );
			PerformBranchInstruction( CFCond, instruction.GetJumpLabelName(), functionName );
			}
			break;
		case 0x91:
			PerformIndirectIndexedWriteInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x92:
			PerformIndirectWriteInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x93:
			PerformIndirectStackWriteInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0x94:
			PerformDirectWriteInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_X, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerX ), m_IRBuilder.CreateLoad( &m_registerY ) );
			break;
		case 0x95:
			PerformDirectWriteInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerX ), m_IRBuilder.CreateLoad( &m_registerA ) );
			break;
		case 0x96:
			PerformDirectWriteInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_X, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerY ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0x97:
			PerformIndirectLongWriteInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerY ) );
			break;
		case 0x98:
			PerformTransferInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_M, &m_registerY, &m_registerA );
			break;
		case 0x99:
			PerformBankWriteInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerY ), m_IRBuilder.CreateLoad( &m_registerA ) );
			break;
		case 0x9a:
			PerformTransferXSInstruction();
			break;
		case 0x9b:
			PerformTransferInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_X, &m_registerX, &m_registerY );
			break;
		case 0x9c:
			PerformBankWriteInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), GetConstant( 0, 16, false ) );
			break;
		case 0x9d:
			PerformBankWriteInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerX ), m_IRBuilder.CreateLoad( &m_registerA ) );
			break;
		case 0x9e:
			PerformBankWriteInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerX ), GetConstant( 0, 16, false ) );
			break;
		case 0x9f:
			PerformLongWriteInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0xa0:
			PerformImmediateReadInstruction( &Recompiler::LDY8, &Recompiler::LDY16, RegisterModeFlag::REGISTER_MODE_FLAG_X, GetConstant( instruction.GetOperand(), 16, false ) );
			break;
		case 0xa1:
			PerformIndexedIndirectReadInstruction( &Recompiler::LDA8, &Recompiler::LDA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xa2:
			PerformImmediateReadInstruction( &Recompiler::LDX8, &Recompiler::LDX16, RegisterModeFlag::REGISTER_MODE_FLAG_X, GetConstant( instruction.GetOperand(), 16, false ) );
			break;
		case 0xa3:
			PerformStackReadInstruction( &Recompiler::LDA8, &Recompiler::LDA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xa4:
			PerformDirectReadInstruction( &Recompiler::LDY8, &Recompiler::LDY16, RegisterModeFlag::REGISTER_MODE_FLAG_X, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xa5:
			PerformDirectReadInstruction( &Recompiler::LDA8, &Recompiler::LDA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xa6:
			PerformDirectReadInstruction( &Recompiler::LDX8, &Recompiler::LDX16, RegisterModeFlag::REGISTER_MODE_FLAG_X, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xa7:
			PerformIndirectLongReadInstruction( &Recompiler::LDA8, &Recompiler::LDA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), GetConstant( 0, 16, false ) );
			break;
		case 0xa8:
			PerformTransferInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_X, &m_registerA, &m_registerY );
			break;
		case 0xa9:
			PerformImmediateReadInstruction( &Recompiler::LDA8, &Recompiler::LDA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ) );
			break;
		case 0xaa:
			PerformTransferInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_X, &m_registerA, &m_registerX );
			break;
		case 0xab:
			PerformPullBInstruction();
			break;
		case 0xac:
			PerformBankReadInstruction( &Recompiler::LDY8, &Recompiler::LDY16, RegisterModeFlag::REGISTER_MODE_FLAG_X, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xad:
			PerformBankReadInstruction( &Recompiler::LDA8, &Recompiler::LDA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xae:
			PerformBankReadInstruction( &Recompiler::LDX8, &Recompiler::LDX16, RegisterModeFlag::REGISTER_MODE_FLAG_X, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xaf:
			PerformLongReadInstruction( &Recompiler::LDA8, &Recompiler::LDA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), GetConstant( 0, 16, false ) );
			break;
		case 0xb0:
			{
			auto CF = m_IRBuilder.CreateLoad( &m_CarryFlag );
			auto CFCond = m_IRBuilder.CreateICmpEQ( CF, GetConstant( 1, 1, false ) );
			PerformBranchInstruction( CFCond, instruction.GetJumpLabelName(), functionName );
			}
			break;
		case 0xb1:
			PerformIndirectIndexedReadInstruction( &Recompiler::LDA8, &Recompiler::LDA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xb2:
			PerformIndirectReadInstruction( &Recompiler::LDA8, &Recompiler::LDA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xb3:
			PerformIndirectStackReadInstruction( &Recompiler::LDA8, &Recompiler::LDA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xb4:
			PerformDirectReadInstruction( &Recompiler::LDY8, &Recompiler::LDY16, RegisterModeFlag::REGISTER_MODE_FLAG_X, GetConstant( instruction.GetOperand(), 16, false ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0xb5:
			PerformDirectReadInstruction( &Recompiler::LDA8, &Recompiler::LDA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0xb6:
			PerformDirectReadInstruction( &Recompiler::LDX8, &Recompiler::LDX16, RegisterModeFlag::REGISTER_MODE_FLAG_X, GetConstant( instruction.GetOperand(), 16, false ), m_IRBuilder.CreateLoad( &m_registerY ) );
			break;
		case 0xb7:
			PerformIndirectLongReadInstruction( &Recompiler::LDA8, &Recompiler::LDA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerY ) );
			break;
		case 0xb8:
			PerformClearFlagInstruction( &m_OverflowFlag );
			break;
		case 0xb9:
			PerformBankReadInstruction( &Recompiler::LDA8, &Recompiler::LDA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ), m_IRBuilder.CreateLoad( &m_registerY ) );
			break;
		case 0xba:
			PerformTransferSXInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_X );
			break;
		case 0xbb:
			PerformTransferInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_X, &m_registerY, &m_registerX );
			break;
		case 0xbc:
			PerformBankReadInstruction( &Recompiler::LDY8, &Recompiler::LDY16, RegisterModeFlag::REGISTER_MODE_FLAG_X, GetConstant( instruction.GetOperand(), 16, false ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0xbd:
			PerformBankReadInstruction( &Recompiler::LDA8, &Recompiler::LDA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0xbe:
			PerformBankReadInstruction( &Recompiler::LDX8, &Recompiler::LDX16, RegisterModeFlag::REGISTER_MODE_FLAG_X, GetConstant( instruction.GetOperand(), 16, false ), m_IRBuilder.CreateLoad( &m_registerY ) );
			break;
		case 0xbf:
			PerformLongReadInstruction( &Recompiler::LDA8, &Recompiler::LDA16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0xc0:
			PerformImmediateReadInstruction( &Recompiler::CPY8, &Recompiler::CPY16, RegisterModeFlag::REGISTER_MODE_FLAG_X, GetConstant( instruction.GetOperand(), 16, false ) );
			break;
		case 0xc1:
			PerformIndexedIndirectReadInstruction( &Recompiler::CMP8, &Recompiler::CMP16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xc2:
			PerformResetPInstruction( GetConstant( instruction.GetOperand(), 8, false ) );
			break;
		case 0xc3:
			PerformStackReadInstruction( &Recompiler::CMP8, &Recompiler::CMP16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xc4:
			PerformDirectReadInstruction( &Recompiler::CPY8, &Recompiler::CPY16, RegisterModeFlag::REGISTER_MODE_FLAG_X, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xc5:
			PerformDirectReadInstruction( &Recompiler::CMP8, &Recompiler::CMP16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xc6:
			PerformDirectModifyInstruction( &Recompiler::DEC8, &Recompiler::DEC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xc7:
			PerformIndirectLongReadInstruction( &Recompiler::CMP8, &Recompiler::CMP16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), GetConstant( 0, 16, false ) );
			break;
		case 0xc8:
			PerformImpliedModifyInstruction( &Recompiler::INC8, &Recompiler::INC16, RegisterModeFlag::REGISTER_MODE_FLAG_X, &m_registerY );
			break;
		case 0xc9:
			PerformImmediateReadInstruction( &Recompiler::CMP8, &Recompiler::CMP16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ) );
			break;
		case 0xca:
			PerformImpliedModifyInstruction( &Recompiler::DEC8, &Recompiler::DEC16, RegisterModeFlag::REGISTER_MODE_FLAG_X, &m_registerX );
			break;
		case 0xcb:
			// WAI - Nothing to do.
			break;
		case 0xcc:
			PerformBankReadInstruction( &Recompiler::CPY8, &Recompiler::CPY16, RegisterModeFlag::REGISTER_MODE_FLAG_X, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xcd:
			PerformBankReadInstruction( &Recompiler::CMP8, &Recompiler::CMP16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xce:
			PerformBankModifyInstruction( &Recompiler::DEC8, &Recompiler::DEC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xcf:
			PerformLongReadInstruction( &Recompiler::CMP8, &Recompiler::CMP16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), GetConstant( 0, 16, false ) );
			break;
		case 0xd0:
			{
			auto ZF = m_IRBuilder.CreateLoad( &m_ZeroFlag );
			auto ZFCond = m_IRBuilder.CreateICmpEQ( ZF, GetConstant( 0, 1, false ) );
			PerformBranchInstruction( ZFCond, instruction.GetJumpLabelName(), functionName );
			}
			break;
		case 0xd1:
			PerformIndirectIndexedReadInstruction( &Recompiler::CMP8, &Recompiler::CMP16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xd2:
			PerformIndirectReadInstruction( &Recompiler::CMP8, &Recompiler::CMP16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xd3:
			PerformIndirectStackReadInstruction( &Recompiler::CMP8, &Recompiler::CMP16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xd4:
			PerformPushEffectiveIndirectAddressInstruction( GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xd5:
			PerformDirectReadInstruction( &Recompiler::CMP8, &Recompiler::CMP16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0xd6:
			PerformBankIndexedModifyInstruction( &Recompiler::DEC8, &Recompiler::DEC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ) );
			break;
		case 0xd7:
			PerformIndirectLongReadInstruction( &Recompiler::CMP8, &Recompiler::CMP16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerY ) );
			break;
		case 0xd8:
			PerformClearFlagInstruction( &m_DecimalFlag );
			break;
		case 0xd9:
			PerformBankReadInstruction( &Recompiler::CMP8, &Recompiler::CMP16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ), m_IRBuilder.CreateLoad( &m_registerY ) );
			break;
		case 0xda:
			PerformPushInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_X, m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0xdb:
			// STP - Nothing to do.
			break;
		case 0xdc:
			PerformJumpIndirectLongInstruction( instruction.GetOffset(), GetConstant( instruction.GetOperand(), 16, false ), functionName );
			break;
		case 0xdd:
			PerformBankReadInstruction( &Recompiler::CMP8, &Recompiler::CMP16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0xde:
			PerformBankIndexedModifyInstruction( &Recompiler::DEC8, &Recompiler::DEC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ) );
			break;
		case 0xdf:
			PerformLongReadInstruction( &Recompiler::CMP8, &Recompiler::CMP16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0xe0:
			PerformImmediateReadInstruction( &Recompiler::CPX8, &Recompiler::CPX16, RegisterModeFlag::REGISTER_MODE_FLAG_X, GetConstant( instruction.GetOperand(), 16, false ) );
			break;
		case 0xe1:
			PerformIndexedIndirectReadInstruction( &Recompiler::SBC8, &Recompiler::SBC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xe2:
			PerformSetPInstruction( GetConstant( instruction.GetOperand(), 8, false ) );
			break;
		case 0xe3:
			PerformStackReadInstruction( &Recompiler::SBC8, &Recompiler::SBC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xe4:
			PerformDirectReadInstruction( &Recompiler::CPX8, &Recompiler::CPX16, RegisterModeFlag::REGISTER_MODE_FLAG_X, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xe5:
			PerformDirectReadInstruction( &Recompiler::SBC8, &Recompiler::SBC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xe6:
			PerformDirectModifyInstruction( &Recompiler::INC8, &Recompiler::INC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xe7:
			PerformIndirectLongReadInstruction( &Recompiler::SBC8, &Recompiler::SBC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), GetConstant( 0, 16, false ) );
			break;
		case 0xe8:
			PerformImpliedModifyInstruction( &Recompiler::INC8, &Recompiler::INC16, RegisterModeFlag::REGISTER_MODE_FLAG_X, &m_registerX );
			break;
		case 0xe9:
			PerformImmediateReadInstruction( &Recompiler::SBC8, &Recompiler::SBC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ) );
			break;
		case 0xea:
			// NOP - Nothing to do.
			break;
		case 0xeb:
			PerformExchangeBAInstruction();
			break;
		case 0xec:
			PerformBankReadInstruction( &Recompiler::CPX8, &Recompiler::CPX16, RegisterModeFlag::REGISTER_MODE_FLAG_X, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xed:
			PerformBankReadInstruction( &Recompiler::SBC8, &Recompiler::SBC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xee:
			PerformBankModifyInstruction( &Recompiler::INC8, &Recompiler::INC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xef:
			PerformLongReadInstruction( &Recompiler::SBC8, &Recompiler::SBC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), GetConstant( 0, 16, false ) );
			break;
		case 0xf0:
			{
			auto ZF = m_IRBuilder.CreateLoad( &m_ZeroFlag );
			auto ZFCond = m_IRBuilder.CreateICmpEQ( ZF, GetConstant( 1, 1, false ) );
			PerformBranchInstruction( ZFCond, instruction.GetJumpLabelName(), functionName );
			}
			break;
		case 0xf1:
			PerformIndirectIndexedReadInstruction( &Recompiler::SBC8, &Recompiler::SBC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xf2:
			PerformIndirectReadInstruction( &Recompiler::SBC8, &Recompiler::SBC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xf3:
			PerformIndirectStackReadInstruction( &Recompiler::SBC8, &Recompiler::SBC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ) );
			break;
		case 0xf4:
			PerformPushEffectiveAddressInstruction( GetConstant( instruction.GetOperand(), 16, false ) );
			break;
		case 0xf5:
			PerformDirectReadInstruction( &Recompiler::SBC8, &Recompiler::SBC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0xf6:
			PerformBankIndexedModifyInstruction( &Recompiler::INC8, &Recompiler::INC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ) );
			break;
		case 0xf7:
			PerformIndirectLongReadInstruction( &Recompiler::SBC8, &Recompiler::SBC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerY ) );
			break;
		case 0xf8:
			PerformSetFlagInstruction( &m_DecimalFlag );
			break;
		case 0xf9:
			PerformBankReadInstruction( &Recompiler::SBC8, &Recompiler::SBC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ), m_IRBuilder.CreateLoad( &m_registerY ) );
			break;
		case 0xfa:
			PerformPullInstruction( RegisterModeFlag::REGISTER_MODE_FLAG_X, &m_registerX );
			break;
		case 0xfb:
			PerformExchangeCEInstruction();
			break;
		case 0xfc:
			PerformCallIndexedIndirectInstruction( instruction.GetOffset(), GetConstant( instruction.GetOperand(), 16, false ) );
			break;
		case 0xfd:
			PerformBankReadInstruction( &Recompiler::SBC8, &Recompiler::SBC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
		case 0xfe:
			PerformBankIndexedModifyInstruction( &Recompiler::INC8, &Recompiler::INC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 16, false ) );
			break;
		case 0xff:
			PerformLongReadInstruction( &Recompiler::SBC8, &Recompiler::SBC16, RegisterModeFlag::REGISTER_MODE_FLAG_M, GetConstant( instruction.GetOperand(), 32, false ), m_IRBuilder.CreateLoad( &m_registerX ) );
			break;
	}
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
		j[ "return_address_manipulation_functions" ].get_to( m_returnAddressManipulationFunctions );

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
						m_Program.emplace_back( Instruction{ current_node[ "Instruction" ][ "offset" ], current_node[ "Instruction" ][ "pc" ], current_node[ "Instruction" ][ "instruction_string" ], current_node[ "Instruction" ][ "opcode" ], current_node[ "Instruction" ][ "operand" ], current_node[ "Instruction" ][ "jump_label_name" ],  current_node[ "Instruction" ][ "operand_size" ],  current_node[ "Instruction" ][ "memory_mode" ], current_node[ "Instruction" ][ "index_mode" ], current_node[ "Instruction" ][ "func_names" ] } );
					}
					else
					{
						m_Program.emplace_back( Instruction{ current_node[ "Instruction" ][ "offset" ], current_node[ "Instruction" ][ "pc" ], current_node[ "Instruction" ][ "instruction_string" ], current_node[ "Instruction" ][ "opcode" ], current_node[ "Instruction" ][ "operand" ], current_node[ "Instruction" ][ "operand_size" ],  current_node[ "Instruction" ][ "memory_mode" ], current_node[ "Instruction" ][ "index_mode" ], current_node[ "Instruction" ][ "func_names" ] } );
					}
				}
				else
				{
					m_Program.emplace_back( Instruction{ current_node[ "Instruction" ][ "offset" ], current_node[ "Instruction" ][ "pc" ], current_node[ "Instruction" ][ "instruction_string" ], current_node[ "Instruction" ][ "opcode" ], current_node[ "Instruction" ][ "memory_mode" ], current_node[ "Instruction" ][ "index_mode" ], current_node[ "Instruction" ][ "func_names" ] } );
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

Recompiler::Instruction::Instruction( const uint32_t offset, const uint32_t pc, const std::string& instructionString, const uint8_t opcode, const uint32_t operand, const uint32_t operand_size, MemoryMode memoryMode, MemoryMode indexMode, const std::set<std::string>& funcNames )
	: m_Offset( offset )
	, m_PC( pc )
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

Recompiler::Instruction::Instruction( const uint32_t offset, const uint32_t pc, const std::string& instructionString, const uint8_t opcode, const uint32_t operand, const std::string& jumpLabelName, const uint32_t operand_size, MemoryMode memoryMode, MemoryMode indexMode, const std::set<std::string>& funcNames )
	: m_Offset( offset )
	, m_PC( pc )
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

Recompiler::Instruction::Instruction( const uint32_t offset, const uint32_t pc, const std::string& instructionString, const uint8_t opcode, MemoryMode memoryMode, MemoryMode indexMode, const std::set<std::string>& funcNames )
	: m_Offset( offset )
	, m_PC( pc )
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
