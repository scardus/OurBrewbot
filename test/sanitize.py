# Extra script for [env:native_asan] in platformio.ini.
#
# Builds the native tests with AddressSanitizer and UndefinedBehaviorSanitizer,
# which stop a test with a report the moment our code reads or writes outside
# a buffer, uses freed memory, or overflows an integer. The flag has to reach
# the linker as well as the compiler, and PlatformIO's build_flags only sends
# it to the compiler - hence this script.
#
# Linux only (run it from WSL): the MinGW gcc used on Windows has no sanitizers.

Import("env")

SANITIZE = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-fno-sanitize-recover=all"]

env.Append(CCFLAGS=SANITIZE, LINKFLAGS=SANITIZE)
