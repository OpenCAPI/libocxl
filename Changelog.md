# 1.1.0
## User facing changes
 - Add support for POWER9 wake_host_thread/wait (requires a compiler with GNU extensions for inline assembler)
 - Generate warnings on ignored return values
 - Use opaque structs rather than void pointers for ocxl handles (this should be transparent to callers)
 - Verified GCC 4-8 & Clang 3.6.2-6.0.1 produce correct machine code for OpenCAPI, and whitelisted them
 - Verify & enforce that we compile with strict ANSI C (2011)

# 1.0.0
 - Initial Release
