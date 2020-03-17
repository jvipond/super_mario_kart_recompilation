#include "Recompiler.hpp"
#include <iostream>

int main( int argc, char** argv ) 
{	
	if ( argc < 3 )
	{
		std::cout << "Recompiler: Recompiler astpath target(native or wasm)" << std::endl;
		return EXIT_FAILURE;
	}

	std::string target(argv[2]);
	if ( target != "native" && target != "wasm" )
	{
		std::cout << "ERROR: target must be native or wasm" << std::endl;
		return EXIT_FAILURE;
	}

	Recompiler rc;
	rc.LoadAST( argv[1] );
	rc.Recompile( target );

	return EXIT_SUCCESS;
}
