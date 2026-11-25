cmake_minimum_required(VERSION 3.20)

project(merlin_clone
    VERSION 0.0.1
    DESCRIPTION "Clean-room Merlin-compatible assembler for Apple II (6502/65C02/65C816)"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(asm816
    src/asm/main.c
    src/asm/assemble.c
    src/asm/line.c
    src/asm/expr.c
    src/asm/fileinfo.c
    src/asm/link.c
    src/asm/listing.c
    src/asm/opcodes.c)
target_include_directories(asm816 PRIVATE src/asm)
target_compile_definitions(asm816 PRIVATE ASM816_VERSION="${PROJECT_VERSION}")

enable_testing()
add_subdirectory(tests)
