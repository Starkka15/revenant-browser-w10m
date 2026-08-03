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
#include <wtf/HashMap.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/CString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringHash.h>

namespace WebCore {
namespace WasmWAMR {

// --- import bridge (scope 2b) --------------------------------------------
// Per-function-import context, referenced by a registered raw native's `attachment`. The trampoline
// reads it to know the arg/result kinds and which JS callback to invoke.
struct ImportCtx {
    WTF_MAKE_STRUCT_FAST_ALLOCATED;
    void* funcCtx { nullptr };      // binding token -> the JS function
    Vector<ValKind> params;
    Vector<ValKind> results;
};

// Owns all state WAMR references for the life of an instance's imports: the NativeSymbol arrays (WAMR
// keeps a pointer to them), the C-string names/signatures inside them, the ImportCtx blocks, and the
// list of (moduleName, symbolsPtr) registrations so the instance can unregister on destroy.
struct ImportBridge {
    WTF_MAKE_STRUCT_FAST_ALLOCATED;
    struct Reg { WTF_MAKE_STRUCT_FAST_ALLOCATED; CString moduleName; Vector<NativeSymbol> symbols; };
    Vector<std::unique_ptr<Reg>> regs;
    Vector<std::unique_ptr<ImportCtx>> ctxs;
    Vector<CString> strings; // keeps symbol/signature C-strings alive
    ~ImportBridge()
    {
        for (auto& r : regs)
            wasm_runtime_unregister_natives(r->moduleName.data(), r->symbols.data());
    }
};

static JsImportCallHook g_jsCallHook = nullptr;
void setJsImportCallHook(JsImportCallHook h) { g_jsCallHook = h; }

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
    void setBridge(std::unique_ptr<ImportBridge>&& b) { m_bridge = WTFMove(b); }
private:
    Module* m_module; // owned by the JS layer; guaranteed to outlive this instance.
    wasm_module_inst_t m_inst { nullptr };
    wasm_exec_env_t m_env { nullptr };
    // Destroyed AFTER the dtor body (deinstantiate) runs -> natives are unregistered only once the
    // instance can no longer call them.
    std::unique_ptr<ImportBridge> m_bridge;
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

// --- imports (scope 2b) ---------------------------------------------------

unsigned importCount(const Module* m)
{
    if (!m)
        return 0;
    return wasm_runtime_get_import_count(m->handle());
}

bool importInfo(const Module* m, unsigned index, String& moduleName, String& fieldName, ExternKind& kind)
{
    if (!m || index >= importCount(m))
        return false;
    wasm_import_t im;
    memset(&im, 0, sizeof(im));
    wasm_runtime_get_import_type(m->handle(), static_cast<int32_t>(index), &im);
    moduleName = String::fromUTF8(im.module_name ? im.module_name : "");
    fieldName = String::fromUTF8(im.name ? im.name : "");
    kind = fromWamrExternKind(im.kind);
    return true;
}

bool importFuncSignature(const Module* m, unsigned index, Vector<ValKind>& params, Vector<ValKind>& results)
{
    if (!m || index >= importCount(m))
        return false;
    wasm_import_t im;
    memset(&im, 0, sizeof(im));
    wasm_runtime_get_import_type(m->handle(), static_cast<int32_t>(index), &im);
    if (im.kind != WASM_IMPORT_EXPORT_KIND_FUNC || !im.u.func_type)
        return false;
    wasm_func_type_t ft = im.u.func_type;
    params.clear();
    results.clear();
    uint32_t np = wasm_func_type_get_param_count(ft);
    uint32_t nr = wasm_func_type_get_result_count(ft);
    for (uint32_t p = 0; p < np; ++p)
        params.append(fromWamrValkind(wasm_func_type_get_param_valkind(ft, p)));
    for (uint32_t r = 0; r < nr; ++r)
        results.append(fromWamrValkind(wasm_func_type_get_result_valkind(ft, r)));
    return true;
}

bool importGlobalType(const Module* m, unsigned index, ValKind& kind, bool& isMutable)
{
    if (!m || index >= importCount(m))
        return false;
    wasm_import_t im;
    memset(&im, 0, sizeof(im));
    wasm_runtime_get_import_type(m->handle(), static_cast<int32_t>(index), &im);
    if (im.kind != WASM_IMPORT_EXPORT_KIND_GLOBAL || !im.u.global_type)
        return false;
    kind = fromWamrValkind(wasm_global_type_get_valkind(im.u.global_type));
    isMutable = wasm_global_type_get_mutable(im.u.global_type);
    return true;
}

// The single raw native every function import resolves to. Each param occupies one uint64 slot in
// |args| (i32/f32 in the low 32 bits); the single result (if any) is written back to args[0]. The
// ImportCtx (via attachment) supplies the value kinds and the JS callback token.
static void jsImportTrampoline(wasm_exec_env_t env, uint64_t* args)
{
    auto* ctx = static_cast<ImportCtx*>(wasm_runtime_get_function_attachment(env));
    unsigned nr = ctx ? ctx->results.size() : 0;
    if (!ctx || !g_jsCallHook) {
        if (nr)
            args[0] = 0;
        return;
    }
    unsigned np = ctx->params.size();
    Vector<Val, 8> in(np);
    for (unsigned i = 0; i < np; ++i) {
        in[i].kind = ctx->params[i];
        switch (ctx->params[i]) {
        case ValKind::I32: in[i].u.i32 = *reinterpret_cast<int32_t*>(&args[i]); break;
        case ValKind::I64: in[i].u.i64 = *reinterpret_cast<int64_t*>(&args[i]); break;
        case ValKind::F32: in[i].u.f32 = *reinterpret_cast<float*>(&args[i]); break;
        case ValKind::F64: in[i].u.f64 = *reinterpret_cast<double*>(&args[i]); break;
        default: in[i].u.i64 = 0; break;
        }
    }
    Val out;
    out.kind = nr ? ctx->results[0] : ValKind::I32;
    out.u.i64 = 0;
    bool ok = g_jsCallHook(ctx->funcCtx, in.data(), np, &out, nr);
    if (!ok) {
        wasm_runtime_set_exception(wasm_runtime_get_module_inst(env), "WebAssembly: JS import threw");
        return;
    }
    if (nr) {
        switch (out.kind) {
        case ValKind::I32: *reinterpret_cast<int32_t*>(&args[0]) = out.u.i32; break;
        case ValKind::I64: *reinterpret_cast<int64_t*>(&args[0]) = out.u.i64; break;
        case ValKind::F32: *reinterpret_cast<float*>(&args[0]) = out.u.f32; break;
        case ValKind::F64: *reinterpret_cast<double*>(&args[0]) = out.u.f64; break;
        default: args[0] = 0; break;
        }
    }
}

static char sigChar(ValKind k)
{
    switch (k) {
    case ValKind::I32: return 'i';
    case ValKind::I64: return 'I';
    case ValKind::F32: return 'f';
    case ValKind::F64: return 'F';
    default: return 'i';
    }
}

Instance* instantiateWithImports(Module* module, Vector<HostImport>&& imports, String& error)
{
    if (!module) {
        error = "no module"_s;
        return nullptr;
    }
    auto bridge = makeUnique<ImportBridge>();

    // Group function-import NativeSymbols by module namespace. Globals/tables/memory imports are not
    // bridged here yet (funcs are the blocker for JS glue / PoW WASM); if the module truly needs them,
    // wasm_runtime_instantiate below will fail to link and we surface that error.
    HashMap<String, std::unique_ptr<ImportBridge::Reg>> byModule;

    unsigned n = importCount(module);
    for (unsigned i = 0; i < n && i < imports.size(); ++i) {
        const HostImport& hi = imports[i];
        if (hi.kind != ExternKind::Func)
            continue;

        auto ctx = makeUnique<ImportCtx>();
        ctx->funcCtx = hi.funcCtx;
        ctx->params = hi.funcParams;
        ctx->results = hi.funcResults;
        ImportCtx* ctxPtr = ctx.get();
        bridge->ctxs.append(WTFMove(ctx));

        // Build the WAMR signature string "(params)result".
        StringBuilder sb;
        sb.append('(');
        for (auto k : hi.funcParams)
            sb.append(sigChar(k));
        sb.append(')');
        if (!hi.funcResults.isEmpty())
            sb.append(sigChar(hi.funcResults[0]));
        bridge->strings.append(sb.toString().utf8());
        const char* sigCstr = bridge->strings.last().data();
        bridge->strings.append(hi.fieldName.utf8());
        const char* nameCstr = bridge->strings.last().data();

        NativeSymbol sym;
        sym.symbol = nameCstr;
        sym.func_ptr = reinterpret_cast<void*>(&jsImportTrampoline);
        sym.signature = sigCstr;
        sym.attachment = ctxPtr;

        auto it = byModule.find(hi.moduleName);
        if (it == byModule.end()) {
            auto reg = makeUnique<ImportBridge::Reg>();
            reg->moduleName = hi.moduleName.utf8();
            reg->symbols.append(sym);
            byModule.set(hi.moduleName, WTFMove(reg));
        } else
            it->value->symbols.append(sym);
    }

    // Register the natives (WAMR references both the symbol array and the strings for the module life).
    for (auto& kv : byModule) {
        ImportBridge::Reg* reg = kv.value.get();
        if (!wasm_runtime_register_natives_raw(reg->moduleName.data(), reg->symbols.data(), reg->symbols.size())) {
            error = "failed to register import functions"_s;
            return nullptr; // bridge dtor unregisters any that DID register
        }
        bridge->regs.append(WTFMove(kv.value));
    }

    // WAMR resolves function imports against the GLOBAL native-symbol list at LOAD time
    // (wasm_loader.c load_function_import -> wasm_resolve_import_func). Our natives are only
    // known now (importObject exists at instantiate, not at compile), so the load-time resolve
    // saw nothing and left func_ptr_linked=NULL. wasm_runtime_instantiate does NOT re-resolve
    // (init_func_ptrs just copies func_ptr_linked), so we must re-run resolution ourselves now
    // that the natives are registered — else every import call traps "unlinked import function".
    wasm_runtime_resolve_symbols(module->handle());

    char err[192] = { 0 };
    wasm_module_inst_t inst = wasm_runtime_instantiate(module->handle(), 32 * 1024, 0, err, sizeof(err));
    if (!inst) {
        error = String::fromUTF8(err);
        if (error.isEmpty())
            error = "failed to instantiate module with imports"_s;
        return nullptr;
    }
    wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, 32 * 1024);
    if (!env) {
        wasm_runtime_deinstantiate(inst);
        error = "failed to create execution environment"_s;
        return nullptr;
    }
    auto* instance = new Instance(module, inst, env);
    instance->setBridge(WTFMove(bridge));
    return instance;
}

// --- exported global / memory / table accessors ---------------------------

bool exportedGlobalType(Instance* instance, const char* name, ValKind& kind, bool& isMutable)
{
    if (!instance || !name)
        return false;
    wasm_global_inst_t g;
    memset(&g, 0, sizeof(g));
    if (!wasm_runtime_get_export_global_inst(instance->inst(), name, &g) || !g.global_data)
        return false;
    kind = fromWamrValkind(g.kind);
    isMutable = g.is_mutable;
    return true;
}

bool exportedGlobalGet(Instance* instance, const char* name, Val& out)
{
    if (!instance || !name)
        return false;
    wasm_global_inst_t g;
    memset(&g, 0, sizeof(g));
    if (!wasm_runtime_get_export_global_inst(instance->inst(), name, &g) || !g.global_data)
        return false;
    out.kind = fromWamrValkind(g.kind);
    switch (g.kind) {
    case WASM_I32: out.u.i32 = *static_cast<int32_t*>(g.global_data); break;
    case WASM_I64: out.u.i64 = *static_cast<int64_t*>(g.global_data); break;
    case WASM_F32: out.u.f32 = *static_cast<float*>(g.global_data); break;
    case WASM_F64: out.u.f64 = *static_cast<double*>(g.global_data); break;
    default: out.u.i64 = 0; break;
    }
    return true;
}

bool exportedGlobalSet(Instance* instance, const char* name, const Val& in)
{
    if (!instance || !name)
        return false;
    wasm_global_inst_t g;
    memset(&g, 0, sizeof(g));
    if (!wasm_runtime_get_export_global_inst(instance->inst(), name, &g) || !g.global_data || !g.is_mutable)
        return false;
    switch (g.kind) {
    case WASM_I32: *static_cast<int32_t*>(g.global_data) = in.u.i32; break;
    case WASM_I64: *static_cast<int64_t*>(g.global_data) = in.u.i64; break;
    case WASM_F32: *static_cast<float*>(g.global_data) = in.u.f32; break;
    case WASM_F64: *static_cast<double*>(g.global_data) = in.u.f64; break;
    default: return false;
    }
    return true;
}

uint32_t memoryPageCount(Instance* instance)
{
    if (!instance)
        return 0;
    wasm_memory_inst_t mem = wasm_runtime_get_default_memory(instance->inst());
    return mem ? static_cast<uint32_t>(wasm_memory_get_cur_page_count(mem)) : 0;
}

uint32_t memoryMaxPageCount(Instance* instance)
{
    if (!instance)
        return 0;
    wasm_memory_inst_t mem = wasm_runtime_get_default_memory(instance->inst());
    return mem ? static_cast<uint32_t>(wasm_memory_get_max_page_count(mem)) : 0;
}

int memoryGrow(Instance* instance, uint32_t deltaPages)
{
    if (!instance)
        return -1;
    uint32_t oldPages = memoryPageCount(instance);
    if (!wasm_runtime_enlarge_memory(instance->inst(), deltaPages))
        return -1;
    return static_cast<int>(oldPages);
}

bool exportedTableInfo(Instance* instance, const char* name, uint32_t& curSize, uint32_t& maxSize)
{
    if (!instance || !name)
        return false;
    wasm_table_inst_t t;
    memset(&t, 0, sizeof(t));
    if (!wasm_runtime_get_export_table_inst(instance->inst(), name, &t))
        return false;
    curSize = t.cur_size;
    maxSize = t.max_size;
    return true;
}

bool exportedTableFuncName(Instance* instance, const char* tableName, uint32_t idx, String& funcName)
{
    // WAMR exposes the function *instance* at a table slot but not a stable export name for it, so we can
    // only report whether the slot is populated. Full funcref<->JS identity round-tripping is a follow-up.
    funcName = String();
    if (!instance || !tableName)
        return false;
    wasm_table_inst_t t;
    memset(&t, 0, sizeof(t));
    if (!wasm_runtime_get_export_table_inst(instance->inst(), tableName, &t) || idx >= t.cur_size)
        return false;
    return wasm_table_get_func_inst(instance->inst(), &t, idx) != nullptr;
}

} // namespace WasmWAMR
} // namespace WebCore
