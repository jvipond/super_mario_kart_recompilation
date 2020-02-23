#include "hardware/hardware.hpp"

int main( int argc, char** argv ) 
{	
	Hardware::GetInstance().PowerOn();
	return 0;
}