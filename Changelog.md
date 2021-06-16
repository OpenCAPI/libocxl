# 1.2.1
 - Set library version correctly
 - Fix test build

# 1.2.0
This is mostly a bug fix release, there's no major new feature:
 - Fix to support devices names with a hexadecimal domain name
 - Allow to override path to read driver info (LIBOCXL_SYSPATH)
 - Documentation enhancements
 - Makefile/build enhancements
 - Add tests for debug AFUs (memcpy, afp)


# 1.1.0
## User facing changes
 - Requires Linux headers >= 4.18 to compile
 - Add support for POWER9 wake_host_thread/wait (requires a compiler with GNU extensions for inline assembler)
 - Generate warnings on ignored return values
 - Use opaque structs rather than void pointers for ocxl handles (this should be transparent to callers)
 - Verified GCC 4-8 & Clang 3.6.2-6.0.1 produce correct machine code for OpenCAPI, and whitelisted them
 - Verify & enforce that we compile with strict ANSI C (2011)

# 1.0.0
 - Initial Release
