/*
 * Revenant: WAMR classic-interpreter wrapper. See WasmWAMR.h.
 */
#include "config.h"
#include "WasmWAMR.h"

// wamr.lib is linked STATICALLY. wasm_export.h defaults WASM_RUNTIME_API_EXTERN to
// __declspec(dllimport) on MSVC, which turns every wasm_* symbol into an __imp_ thunk that
// a static archive cannot satisfy (LNK2019 __imp_wasm_*). Force it empty for direct static
// linkage so the symbols resolve straight from wamr.lib.
#define WASM_RUNTIME_API_EXTERN
#include <wasm_export.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/StdLibExtras.h>

namespace WebCore {
namespace WasmWAMR {

// --- opaque handles -------------------------------------------------------

class Module {
public:
    Module(wasm_module_t m, Vector<uint8_t>&& bytes)
        : m_module(m), m_bytes(WTFMove(bytes)) { }
    ~Module() { if (m_module) wasm_runtime_unload(m_module); }
    wasm_module_t handle() const { return m_module; }
private:
    wasm_module_t m_module { nullptr };
    Vector<uint8_t> m_bytes; // WAMR references the buffer for the module lifetime.
};

class Instance {
public:
    Instance(Module* module, wasm_module_inst_t inst, wasm_exec_env_t env)
        : m_module(module), m_inst(inst), m_env(env) { }
    ~Instance()
    {
        if (m_env)
            wasm_runtime_destroy_exec_env(m_env);
        if (m_inst)
            wasm_runtime_deinstantiate(m_inst);
    }
    Module* module() const { return m_module; }
    wasm_module_inst_t inst() const { return m_inst; }
    wasm_exec_env_t env() const { return m_env; }
private:
    Module* m_module; // owned by the JS layer; guaranteed to outlive this instance.
    wasm_module_inst_t m_inst { nullptr };
    wasm_exec_env_t m_env { nullptr };
};

// --- helpers --------------------------------------------------------------

static ValKind fromWamrValkind(uint8_t k)
{
    switch (k) {
    case WASM_I32: return ValKind::I32;
    case WASM_I64: return ValKind::I64;
    case WASM_F32: return ValKind::F32;
    case WASM_F64: return ValKind::F64;
    default: return ValKind::Ref;
    }
}

static ExternKind fromWamrExternKind(uint8_t k)
{
    switch (k) {
    case WASM_IMPORT_EXPORT_KIND_FUNC: return ExternKind::Func;
    case WASM_IMPORT_EXPORT_KIND_TABLE: return ExternKind::Table;
    case WASM_IMPORT_EXPORT_KIND_MEMORY: return ExternKind::Memory;
    case WASM_IMPORT_EXPORT_KIND_GLOBAL: return ExternKind::Global;
    default: return ExternKind::Unknown;
    }
}

// --- runtime init ---------------------------------------------------------

bool ensureRuntime()
{
    static bool s_ok = [] {
        RuntimeInitArgs args;
        memset(&args, 0, sizeof(args));
        args.mem_alloc_type = Alloc_With_System_Allocator; // plain malloc/free — no fixed pool.
        return wasm_runtime_full_init(&args);
    }();
    return s_ok;
}

// --- API ------------------------------------------------------------------

bool validate(const uint8_t* bytes, size_t length)
{
    if (!ensureRuntime() || !bytes || !length)
        return false;
    char err[128] = { 0 };
    // load with a throwaway copy; unload immediately.
    Vector<uint8_t> copy;
    copy.append(bytes, length);
    wasm_module_t m = wasm_runtime_load(copy.data(), static_cast<uint32_t>(copy.size()), err, sizeof(err));
    if (!m)
        return false;
    wasm_runtime_unload(m);
    return true;
}

Module* compile(const uint8_t* bytes, size_t length, String& error)
{
    if (!ensureRuntime()) {
        error = "WebAssembly runtime init failed"_s;
        return nullptr;
    }
    if (!bytes || !length) {
        error = "empty module"_s;
        return nullptr;
    }
    Vector<uint8_t> owned;
    owned.append(bytes, length);
    char err[128] = { 0 };
    wasm_module_t m = wasm_runtime_load(owned.data(), static_cast<uint32_t>(owned.size()), err, sizeof(err));
    if (!m) {
        error = String::fromUTF8(err);
        if (error.isEmpty())
            error = "failed to compile module"_s;
        return nullptr;
    }
    return new Module(m, WTFMove(owned));
}

void destroy(Module* m) { delete m; }

unsigned exportCount(const Module* m)
{
    if (!m)
        return 0;
    return wasm_runtime_get_export_count(m->handle());
}

bool exportInfo(const Module* m, unsigned index, String& name, ExternKind& kind)
{
    if (!m || index >= exportCount(m))
        return false;
    wasm_export_t e;
    memset(&e, 0, sizeof(e));
    wasm_runtime_get_export_type(m->handle(), static_cast<int32_t>(index), &e); // returns void; fills e
    name = String::fromUTF8(e.name ? e.name : "");
    kind = fromWamrExternKind(e.kind);
    return true;
}

bool hasImports(const Module* m)
{
    if (!m)
        return false;
    return wasm_runtime_get_import_count(m->handle()) > 0;
}

Instance* instantiate(Module* module, String& error)
{
    if (!module) {
        error = "no module"_s;
        return nullptr;
    }
    char err[128] = { 0 };
    // 32KB wasm stack, 0 host-managed heap (module supplies its own memory).
    wasm_module_inst_t inst = wasm_runtime_instantiate(module->handle(), 32 * 1024, 0, err, sizeof(err));
    if (!inst) {
        error = String::fromUTF8(err);
        if (error.isEmpty())
            error = "failed to instantiate module"_s;
        return nullptr;
    }
    wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, 32 * 1024);
    if (!env) {
        wasm_runtime_deinstantiate(inst);
        error = "failed to create execution environment"_s;
        return nullptr;
    }
    return new Instance(module, inst, env);
}

void destroy(Instance* i) { delete i; }

bool functionSignature(Instance* instance, const char* name, Vector<ValKind>& params, Vector<ValKind>& results)
{
    if (!instance || !name)
        return false;
    // Find the export by name whose kind is Func, then read its func_type.
    Module* m = instance->module();
    unsigned count = exportCount(m);
    for (unsigned i = 0; i < count; ++i) {
        wasm_export_t e;
        memset(&e, 0, sizeof(e));
        wasm_runtime_get_export_type(m->handle(), static_cast<int32_t>(i), &e); // returns void; fills e
        if (e.kind != WASM_IMPORT_EXPORT_KIND_FUNC || !e.name || strcmp(e.name, name))
            continue;
        wasm_func_type_t ft = e.u.func_type;
        if (!ft)
            return false;
        uint32_t np = wasm_func_type_get_param_count(ft);
        uint32_t nr = wasm_func_type_get_result_count(ft);
        params.clear();
        results.clear();
        for (uint32_t p = 0; p < np; ++p)
            params.append(fromWamrValkind(wasm_func_type_get_param_valkind(ft, p)));
        for (uint32_t r = 0; r < nr; ++r)
            results.append(fromWamrValkind(wasm_func_type_get_result_valkind(ft, r)));
        return true;
    }
    return false;
}

bool callFunction(Instance* instance, const char* name, const Val* args, unsigned argCount, Val* results, unsigned resultCount, String& error)
{
    if (!instance || !name) {
        error = "invalid call"_s;
        return false;
    }
    wasm_function_inst_t func = wasm_runtime_lookup_function(instance->inst(), name);
    if (!func) {
        error = "exported function not found"_s;
        return false;
    }

    Vector<wasm_val_t> wargs(argCount);
    for (unsigned i = 0; i < argCount; ++i) {
        wasm_val_t& w = wargs[i];
        memset(&w, 0, sizeof(w));
        switch (args[i].kind) {
        case ValKind::I32: w.kind = WASM_I32; w.of.i32 = args[i].u.i32; break;
        case ValKind::I64: w.kind = WASM_I64; w.of.i64 = args[i].u.i64; break;
        case ValKind::F32: w.kind = WASM_F32; w.of.f32 = args[i].u.f32; break;
        case ValKind::F64: w.kind = WASM_F64; w.of.f64 = args[i].u.f64; break;
        default: w.kind = WASM_I32; w.of.i32 = 0; break; // ref args unsupported in 2a
        }
    }
    Vector<wasm_val_t> wresults(resultCount ? resultCount : 1);
    for (auto& r : wresults)
        memset(&r, 0, sizeof(r));

    bool ok = wasm_runtime_call_wasm_a(instance->env(), func,
        resultCount, resultCount ? wresults.data() : nullptr,
        argCount, argCount ? wargs.data() : nullptr);
    if (!ok) {
        const char* ex = wasm_runtime_get_exception(instance->inst());
        error = String::fromUTF8(ex ? ex : "wasm trap");
        return false;
    }
    for (unsigned i = 0; i < resultCount; ++i) {
        results[i].kind = fromWamrValkind(wresults[i].kind);
        switch (wresults[i].kind) {
        case WASM_I32: results[i].u.i32 = wresults[i].of.i32; break;
        case WASM_I64: results[i].u.i64 = wresults[i].of.i64; break;
        case WASM_F32: results[i].u.f32 = wresults[i].of.f32; break;
        case WASM_F64: results[i].u.f64 = wresults[i].of.f64; break;
        default: results[i].u.i64 = 0; break;
        }
    }
    return true;
}

uint8_t* memoryData(Instance* instance, size_t& byteLength)
{
    byteLength = 0;
    if (!instance)
        return nullptr;
    wasm_memory_inst_t mem = wasm_runtime_get_default_memory(instance->inst());
    if (!mem)
        return nullptr;
    void* base = wasm_memory_get_base_address(mem);
    uint64_t pages = wasm_memory_get_cur_page_count(mem);
    uint64_t perPage = wasm_memory_get_bytes_per_page(mem);
    byteLength = static_cast<size_t>(pages * perPage);
    return static_cast<uint8_t*>(base);
}

} // namespace WasmWAMR
} // namespace WebCore
