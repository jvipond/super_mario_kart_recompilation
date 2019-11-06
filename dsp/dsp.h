/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _DSP1_H_
#define _DSP1_H_

#include <cstdint>

struct SDSP1
{
	bool	waiting4command;
	bool	first_parameter;
	uint8_t	command;
	uint32_t	in_count;
	uint32_t	in_index;
	uint32_t	out_count;
	uint32_t	out_index;
	uint8_t	parameters[512];
	uint8_t	output[512];

	int16_t	CentreX;
	int16_t	CentreY;
	int16_t	VOffset;

	int16_t	VPlane_C;
	int16_t	VPlane_E;

	// Azimuth and Zenith angles
	int16_t	SinAas;
	int16_t	CosAas;
	int16_t	SinAzs;
	int16_t	CosAzs;

	// Clipped Zenith angle
	int16_t	SinAZS;
	int16_t	CosAZS;
	int16_t	SecAZS_C1;
	int16_t	SecAZS_E1;
	int16_t	SecAZS_C2;
	int16_t	SecAZS_E2;

	int16_t	Nx;
	int16_t	Ny;
	int16_t	Nz;
	int16_t	Gx;
	int16_t	Gy;
	int16_t	Gz;
	int16_t	C_Les;
	int16_t	E_Les;
	int16_t	G_Les;

	int16_t	matrixA[3][3];
	int16_t	matrixB[3][3];
	int16_t	matrixC[3][3];

	int16_t	Op00Multiplicand;
	int16_t	Op00Multiplier;
	int16_t	Op00Result;

	int16_t	Op20Multiplicand;
	int16_t	Op20Multiplier;
	int16_t	Op20Result;

	int16_t	Op10Coefficient;
	int16_t	Op10Exponent;
	int16_t	Op10CoefficientR;
	int16_t	Op10ExponentR;

	int16_t	Op04Angle;
	int16_t	Op04Radius;
	int16_t	Op04Sin;
	int16_t	Op04Cos;

	int16_t	Op0CA;
	int16_t	Op0CX1;
	int16_t	Op0CY1;
	int16_t	Op0CX2;
	int16_t	Op0CY2;

	int16_t	Op02FX;
	int16_t	Op02FY;
	int16_t	Op02FZ;
	int16_t	Op02LFE;
	int16_t	Op02LES;
	int16_t	Op02AAS;
	int16_t	Op02AZS;
	int16_t	Op02VOF;
	int16_t	Op02VVA;
	int16_t	Op02CX;
	int16_t	Op02CY;

	int16_t	Op0AVS;
	int16_t	Op0AA;
	int16_t	Op0AB;
	int16_t	Op0AC;
	int16_t	Op0AD;

	int16_t	Op06X;
	int16_t	Op06Y;
	int16_t	Op06Z;
	int16_t	Op06H;
	int16_t	Op06V;
	int16_t	Op06M;

	int16_t	Op01m;
	int16_t	Op01Zr;
	int16_t	Op01Xr;
	int16_t	Op01Yr;

	int16_t	Op11m;
	int16_t	Op11Zr;
	int16_t	Op11Xr;
	int16_t	Op11Yr;

	int16_t	Op21m;
	int16_t	Op21Zr;
	int16_t	Op21Xr;
	int16_t	Op21Yr;

	int16_t	Op0DX;
	int16_t	Op0DY;
	int16_t	Op0DZ;
	int16_t	Op0DF;
	int16_t	Op0DL;
	int16_t	Op0DU;

	int16_t	Op1DX;
	int16_t	Op1DY;
	int16_t	Op1DZ;
	int16_t	Op1DF;
	int16_t	Op1DL;
	int16_t	Op1DU;

	int16_t	Op2DX;
	int16_t	Op2DY;
	int16_t	Op2DZ;
	int16_t	Op2DF;
	int16_t	Op2DL;
	int16_t	Op2DU;

	int16_t	Op03F;
	int16_t	Op03L;
	int16_t	Op03U;
	int16_t	Op03X;
	int16_t	Op03Y;
	int16_t	Op03Z;

	int16_t	Op13F;
	int16_t	Op13L;
	int16_t	Op13U;
	int16_t	Op13X;
	int16_t	Op13Y;
	int16_t	Op13Z;

	int16_t	Op23F;
	int16_t	Op23L;
	int16_t	Op23U;
	int16_t	Op23X;
	int16_t	Op23Y;
	int16_t	Op23Z;

	int16_t	Op14Zr;
	int16_t	Op14Xr;
	int16_t	Op14Yr;
	int16_t	Op14U;
	int16_t	Op14F;
	int16_t	Op14L;
	int16_t	Op14Zrr;
	int16_t	Op14Xrr;
	int16_t	Op14Yrr;

	int16_t	Op0EH;
	int16_t	Op0EV;
	int16_t	Op0EX;
	int16_t	Op0EY;

	int16_t	Op0BX;
	int16_t	Op0BY;
	int16_t	Op0BZ;
	int16_t	Op0BS;

	int16_t	Op1BX;
	int16_t	Op1BY;
	int16_t	Op1BZ;
	int16_t	Op1BS;

	int16_t	Op2BX;
	int16_t	Op2BY;
	int16_t	Op2BZ;
	int16_t	Op2BS;

	int16_t	Op28X;
	int16_t	Op28Y;
	int16_t	Op28Z;
	int16_t	Op28R;

	int16_t	Op1CX;
	int16_t	Op1CY;
	int16_t	Op1CZ;
	int16_t	Op1CXBR;
	int16_t	Op1CYBR;
	int16_t	Op1CZBR;
	int16_t	Op1CXAR;
	int16_t	Op1CYAR;
	int16_t	Op1CZAR;
	int16_t	Op1CX1;
	int16_t	Op1CY1;
	int16_t	Op1CZ1;
	int16_t	Op1CX2;
	int16_t	Op1CY2;
	int16_t	Op1CZ2;

	uint16_t	Op0FRamsize;
	uint16_t	Op0FPass;

	int16_t	Op2FUnknown;
	int16_t	Op2FSize;

	int16_t	Op08X;
	int16_t	Op08Y;
	int16_t	Op08Z;
	int16_t	Op08Ll;
	int16_t	Op08Lh;

	int16_t	Op18X;
	int16_t	Op18Y;
	int16_t	Op18Z;
	int16_t	Op18R;
	int16_t	Op18D;

	int16_t	Op38X;
	int16_t	Op38Y;
	int16_t	Op38Z;
	int16_t	Op38R;
	int16_t	Op38D;
};

extern struct SDSP1	DSP1;

void ResetDSP (void);
uint8_t DSP1GetByte (uint32_t addr);
void DSP1SetByte ( uint32_t addr, uint8_t byte );

#endif
