# super_mario_kart_recompilation
Attempt to recompile SNES Super Mario Kart (USA).sfc to x86-64 using LLVM inspired by [Statically Recompiling NES Games into Native Executables with LLVM and Go](https://andrewkelley.me/post/jamulator.html)

To build first clone [super_mario_kart_disassembly](https://github.com/jvipond/super_mario_kart_disassembly) and follow the instructions there. After running the python script you should have a generated file `super_mario_kart_ast.json`.

Then to build on Windows:
1. Clone this repository and create directory called `build`.
2. `cd build` and run `cmake .. -A x64 -DCMAKE_PREFIX_PATH=PATH\TO\LLVM` specifiying the path to the llvm libraries on your machine.
3. Copy `super_mario_kart_ast.json` into the `build` directory and load `super_mario_kart_recompilation.sln` into Visual Studio.
4. Set `smk` as startup project project and build in Release.

To build on Linux:
1. Clone this repository and create directory called `build`.
2. `cd build` and run `cmake ..`.
3. `make` to build.
4. Copy `super_mario_kart_ast.json` into the `build` directory and execute `./smk`.

At the moment it doesn't do anthing useful it just sets up the start of allocating some registers and adding basic blocks for all the program labels.
