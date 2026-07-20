# Revenant: WebAssembly Micro Runtime (WAMR) as an ARM32 UWP static lib.
# Classic interpreter only, pure-C native-call trampoline (no armasm), software
# bounds checks (no mmap guard pages / signal handlers — UWP-safe). This backs
# our own window.WebAssembly binding; JSC's own WASM engine stays disabled.
cmake_minimum_required(VERSION 3.14)
project(wamr_uwp C CXX)

set(WAMR_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../wamr)

# --- target / platform ---
set(WAMR_BUILD_PLATFORM "windows")
set(WAMR_BUILD_TARGET   "THUMBV7_VFP")   # Lumia 640 XL: ARMv7 Thumb-2 + VFP/NEON

# --- engine tiers: classic interpreter ONLY ---
set(WAMR_BUILD_INTERP      1)
set(WAMR_BUILD_FAST_INTERP 0)            # fast interp uses computed-goto (GCC/Clang) — MSVC-unsafe
set(WAMR_BUILD_AOT         0)
set(WAMR_BUILD_JIT         0)

# --- portability: no arch asm, no hardware traps (UWP has neither cleanly) ---
set(WAMR_BUILD_INVOKE_NATIVE_GENERAL 1)  # pure-C native-call trampoline (invokeNative_general.c)
set(WAMR_DISABLE_HW_BOUND_CHECK       1) # software wasm-memory bounds checks
set(WAMR_DISABLE_STACK_HW_BOUND_CHECK 1)

# --- we are a browser host, not WASI: imports come from JS, not libc ---
set(WAMR_BUILD_LIBC_BUILTIN 0)
set(WAMR_BUILD_LIBC_WASI    0)
set(WAMR_BUILD_LIB_PTHREAD  0)
set(WAMR_BUILD_MULTI_MODULE 0)

# --- spec features worth having (cheap in the interpreter) ---
set(WAMR_BUILD_BULK_MEMORY 1)
set(WAMR_BUILD_REF_TYPES   1)
set(WAMR_BUILD_SIMD        0)            # 128-bit SIMD off for now (interp-heavy)

include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)

add_library(wamr STATIC ${WAMR_RUNTIME_LIB_SOURCE})
target_include_directories(wamr PUBLIC ${WAMR_ROOT_DIR}/core/iwasm/include)
