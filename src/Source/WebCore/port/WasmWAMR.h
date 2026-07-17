/*
 * Revenant: thin C++ wrapper around the WebAssembly Micro Runtime (WAMR) classic
 * interpreter. Keeps WAMR's C API (wasm_export.h) OUT of the JSC binding layer —
 * the binding sees only neutral types (WasmWAMR::Val, opaque Module/Instance) and
 * does JSValue<->Val marshalling itself. Backs our own window.WebAssembly because
 * JSC's native WASM engine is 64-bit-only (disabled on ARM32). No stubs — real
 * interpreter execution.
 */
#pragma once

#include <wtf/Forward.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace WebCore {
namespace WasmWAMR {

enum class ValKind : uint8_t { I32, I64, F32, F64, Ref /* externref/funcref — 2b */ };

struct Val {
    ValKind kind { ValKind::I32 };
    union {
        int32_t i32;
        int64_t i64;
        float f32;
        double f64;
    } u { .i64 = 0 };
};

// WASM external kinds as reported by WAMR (wasm_import_export_kind_t).
enum class ExternKind : uint8_t { Func = 0, Table = 1, Memory = 2, Global = 3, Unknown = 255 };

class Module;
class Instance;

// Initialize the WAMR runtime once (idempotent). Returns false if init fails.
bool ensureRuntime();

// Spec WebAssembly.validate — true if the bytes are a well-formed module.
bool validate(const uint8_t* bytes, size_t length);

// Compile (WAMR "load"). Retains its own copy of the bytes for the module lifetime.
// Returns nullptr and fills |error| on failure. Caller owns the returned Module*.
Module* compile(const uint8_t* bytes, size_t length, String& error);
void destroy(Module*);

unsigned exportCount(const Module*);
// Fills name+kind for export |index|. Returns false if out of range.
bool exportInfo(const Module*, unsigned index, String& name, ExternKind&);

// Whether the module declares any imports (2a can only instantiate import-free modules).
bool hasImports(const Module*);

// Instantiate. Returns nullptr and fills |error| on failure (incl. missing imports).
Instance* instantiate(Module*, String& error);
void destroy(Instance*);

// Look up an exported function's signature. Returns false if not an exported function.
bool functionSignature(Instance*, const char* name, Vector<ValKind>& params, Vector<ValKind>& results);

// Call an exported function. args/results sized to the signature. Fills |error| on trap.
bool callFunction(Instance*, const char* name, const Val* args, unsigned argCount, Val* results, unsigned resultCount, String& error);

// Native base + byte length of the instance's default linear memory (index 0).
// Returns nullptr if the module has no memory. The pointer is valid until the
// instance is destroyed or the memory grows (grow => callers must re-fetch).
uint8_t* memoryData(Instance*, size_t& byteLength);

} // namespace WasmWAMR
} // namespace WebCore
