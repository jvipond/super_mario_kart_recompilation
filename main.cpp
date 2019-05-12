#include "Recompiler.hpp"

int main( int argc, char** argv ) 
{	
	Recompiler rc;
	rc.LoadAST( "super_mario_kart_ast.json" );
	rc.Recompile();
	return 0;
}