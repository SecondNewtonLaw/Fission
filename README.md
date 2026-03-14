# Fission

Fission is a Luau decompiler built for my closed source project, **RbxCli**, however it originally was meant to be an Open Source decompiler fully written in C++ to experiment and built upon already existing decompilers available as Open Source.

The decompiler takes no code from any other decompiler, however it has references to existing Luau source code where applicable, as there's many seemingly 'magical' things only achievable looking at the C++ source.

This project has been finally open sourced after around 2 months of work in and out of the project.

While it is able to decompile plenty of samples, it has a high number of problems when dealing with control flow and loops, which are being worked on, however due to the nature of decompilation, the fix for one thing can be terrible for another.

### Planned changes
- Adding unit tests
  - We have to make sure the code we are writing actually decompiles a sample back to an expected state after each change, else we will have terrible support.

Credits to all contributors!