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

// --- imports (scope 2b): instantiate modules that declare imports ---------
// The binding enumerates the module's imports, resolves each from the JS importObject, and passes the
// resolved set to instantiateWithImports(). Function imports are bridged back into JS via a global hook.

unsigned importCount(const Module*);
// Fills moduleName+fieldName+kind for import |index|. Returns false if out of range.
bool importInfo(const Module*, unsigned index, String& moduleName, String& fieldName, ExternKind&);
// For a function import, fills its param/result value kinds. Returns false if not a function import.
bool importFuncSignature(const Module*, unsigned index, Vector<ValKind>& params, Vector<ValKind>& results);
// For a global import, fills its value kind + mutability. Returns false if not a global import.
bool importGlobalType(const Module*, unsigned index, ValKind&, bool& isMutable);

// One host-provided import, in the module's import order.
struct HostImport {
    ExternKind kind { ExternKind::Unknown };
    // Func: opaque token handed back to the JS-call hook when WASM invokes this import.
    void* funcCtx { nullptr };
    Vector<ValKind> funcParams;
    Vector<ValKind> funcResults;
    // Global: the initial value (immutable globals only for now).
    Val globalValue;
    String moduleName; // filled by the binding (module import namespace, e.g. "env")
    String fieldName;  // filled by the binding (import field name)
};

// Set once by the binding. Invoked ON THE THREAD RUNNING THE WASM (a worker thread for Turnstile),
// from inside the WAMR interpreter, when WASM calls a function import. Marshals params->JS, calls the
// stored JS function, marshals the JS result back. Returns false to signal a JS exception (=> WASM trap).
using JsImportCallHook = bool (*)(void* funcCtx, const Val* params, unsigned nparams, Val* results, unsigned nresults);
void setJsImportCallHook(JsImportCallHook);

// Instantiate a module WITH imports. |imports| is in module import order (same order importInfo reports).
// Returns nullptr and fills |error| on failure. The Instance owns the import bridge state.
Instance* instantiateWithImports(Module*, Vector<HostImport>&& imports, String& error);

// --- exported global / memory / table accessors (for the JS Global/Memory/Table objects) ----------

// Read/write an exported global by name. Returns false if there is no such exported global.
bool exportedGlobalGet(Instance*, const char* name, Val& out);
bool exportedGlobalSet(Instance*, const char* name, const Val& in);
// Type of an exported global. Returns false if not found.
bool exportedGlobalType(Instance*, const char* name, ValKind&, bool& isMutable);

// Grow the default linear memory by |deltaPages|. Returns the OLD page count, or -1 on failure.
// (memoryData() must be re-fetched after a successful grow: WAMR may relocate the buffer.)
int memoryGrow(Instance*, uint32_t deltaPages);
// Current / maximum page counts of the default memory (0 max = unlimited-ish). WASM page = 64KiB.
uint32_t memoryPageCount(Instance*);
uint32_t memoryMaxPageCount(Instance*);

// Exported table current/max element counts. Returns false if there is no such exported table.
bool exportedTableInfo(Instance*, const char* name, uint32_t& curSize, uint32_t& maxSize);
// Name of the function referenced at table[idx], or empty if none/not a funcref. (Read-only: WAMR
// exposes table func lookup but not host funcref insertion in the classic interp; set is best-effort.)
bool exportedTableFuncName(Instance*, const char* tableName, uint32_t idx, String& funcName);

} // namespace WasmWAMR
} // namespace WebCore
