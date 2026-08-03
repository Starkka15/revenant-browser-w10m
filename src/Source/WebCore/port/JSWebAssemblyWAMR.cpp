/*
 * Revenant: window.WebAssembly backed by the WAMR classic interpreter.
 *
 * Scope 2a (this file): WebAssembly.validate / .compile / .instantiate returning
 * real, executable Module/Instance objects. Instance.exports exposes exported
 * functions (callable — i32/f32/f64 marshalling) and exported memory (.buffer as a
 * zero-copy ArrayBuffer over WAMR linear memory). Import-free modules only.
 * Scope 2b (later): Module/Instance/Memory/Table/Global constructors, JS-imported
 * functions (register_natives bridge), i64<->BigInt at the boundary, worker scopes.
 */
#include "config.h"
#include "JSWebAssemblyWAMR.h"

#include "WasmWAMR.h"
#include <JavaScriptCore/ArgList.h>
#include <JavaScriptCore/ArrayBuffer.h>
#include <JavaScriptCore/ArrayBufferView.h>
#include <JavaScriptCore/CallData.h>
#include <JavaScriptCore/CatchScope.h>
#include <JavaScriptCore/Strong.h>
#include <JavaScriptCore/StrongInlines.h>
#include <JavaScriptCore/Error.h>
#include <JavaScriptCore/JSArrayBuffer.h>
#include <JavaScriptCore/JSArrayBufferView.h>
#include <JavaScriptCore/JSCInlines.h>
#include <JavaScriptCore/JSNativeStdFunction.h>
#include <JavaScriptCore/JSPromise.h>
#include <JavaScriptCore/ObjectConstructor.h>
#include <JavaScriptCore/ThrowScope.h>
#include <wtf/SharedTask.h>

namespace WebCore {

using namespace JSC;

// ---- WASM function imports (scope 2b) -----------------------------------
// One imported JS function bound to a module instance. Held (owned) by the JSWasmInstance so it lives
// as long as the WASM instance; the Strong ref keeps the JS function alive across WASM execution. The
// address of this struct is the opaque funcCtx WAMR hands back to jsImportCallHook when WASM calls it.
struct JsImportFunc {
    WTF_MAKE_STRUCT_FAST_ALLOCATED;
    JSGlobalObject* globalObject { nullptr };
    Strong<JSObject> function;
    Vector<WasmWAMR::ValKind> params;
    Vector<WasmWAMR::ValKind> results;
};

// Invoked from inside the WAMR interpreter (on whatever thread runs the WASM — a worker thread for
// Turnstile) when WASM calls a function import. Marshals params -> JS, calls the JS function, marshals
// the result back. Returns false on a JS exception so WAMR traps.
static bool jsImportCallHook(void* funcCtx, const WasmWAMR::Val* params, unsigned np, WasmWAMR::Val* results, unsigned nr)
{
    auto* ic = static_cast<JsImportFunc*>(funcCtx);
    if (!ic || !ic->function)
        return false;
    JSGlobalObject* g = ic->globalObject;
    VM& vm = g->vm();
    JSLockHolder lock(vm); // re-entrant; we are already inside JS on this thread
    auto scope = DECLARE_CATCH_SCOPE(vm);

    MarkedArgumentBuffer callArgs;
    for (unsigned i = 0; i < np; ++i) {
        switch (params[i].kind) {
        case WasmWAMR::ValKind::I32: callArgs.append(jsNumber(params[i].u.i32)); break;
        case WasmWAMR::ValKind::I64: callArgs.append(jsNumber(static_cast<double>(params[i].u.i64))); break; // 2b: BigInt
        case WasmWAMR::ValKind::F32: callArgs.append(jsNumber(purifyNaN(params[i].u.f32))); break;
        case WasmWAMR::ValKind::F64: callArgs.append(jsNumber(purifyNaN(params[i].u.f64))); break;
        default: callArgs.append(jsNumber(0)); break;
        }
    }
    if (callArgs.hasOverflowed())
        return false;

    JSObject* fn = ic->function.get();
    auto callData = getCallData(vm, fn);
    if (callData.type == CallData::Type::None)
        return false;
    JSValue r = JSC::call(g, fn, callData, jsUndefined(), callArgs);
    if (scope.exception()) {
        scope.clearException();
        return false;
    }
    if (nr) {
        switch (results[0].kind) {
        case WasmWAMR::ValKind::I32: results[0].u.i32 = r.toInt32(g); break;
        case WasmWAMR::ValKind::I64: results[0].u.i64 = static_cast<int64_t>(r.toNumber(g)); break; // 2b: BigInt
        case WasmWAMR::ValKind::F32: results[0].u.f32 = static_cast<float>(r.toNumber(g)); break;
        case WasmWAMR::ValKind::F64: results[0].u.f64 = r.toNumber(g); break;
        default: results[0].u.i64 = 0; break;
        }
        if (scope.exception()) {
            scope.clearException();
            return false;
        }
    }
    return true;
}

// ---- GC cells wrapping the WAMR handles ---------------------------------

class JSWasmModule final : public JSDestructibleObject {
    using Base = JSDestructibleObject;
public:
    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.destructibleObjectSpace(); }

    static Structure* createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
    {
        return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
    }

    static JSWasmModule* create(VM& vm, JSGlobalObject* globalObject, WasmWAMR::Module* module)
    {
        Structure* structure = createStructure(vm, globalObject, globalObject->objectPrototype());
        JSWasmModule* cell = new (NotNull, allocateCell<JSWasmModule>(vm)) JSWasmModule(vm, structure, module);
        cell->finishCreation(vm);
        return cell;
    }

    static void destroy(JSCell* cell) { static_cast<JSWasmModule*>(cell)->JSWasmModule::~JSWasmModule(); }

    WasmWAMR::Module* module() const { return m_module; }

    DECLARE_INFO;

private:
    JSWasmModule(VM& vm, Structure* structure, WasmWAMR::Module* module)
        : Base(vm, structure), m_module(module) { }
    ~JSWasmModule() { if (m_module) WasmWAMR::destroy(m_module); }

    WasmWAMR::Module* m_module { nullptr };
};

class JSWasmInstance final : public JSDestructibleObject {
    using Base = JSDestructibleObject;
public:
    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.destructibleObjectSpace(); }

    static Structure* createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
    {
        return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
    }

    static JSWasmInstance* create(VM& vm, JSGlobalObject* globalObject, JSWasmModule* module, WasmWAMR::Instance* instance)
    {
        Structure* structure = createStructure(vm, globalObject, globalObject->objectPrototype());
        JSWasmInstance* cell = new (NotNull, allocateCell<JSWasmInstance>(vm)) JSWasmInstance(vm, structure, instance);
        cell->finishCreation(vm);
        cell->m_module.set(vm, cell, module);
        return cell;
    }

    static void destroy(JSCell* cell) { static_cast<JSWasmInstance*>(cell)->JSWasmInstance::~JSWasmInstance(); }

    DECLARE_VISIT_CHILDREN;

    WasmWAMR::Instance* instance() const { return m_instance; }
    JSObject* exports() const { return m_exports.get(); }
    void setExports(VM& vm, JSObject* exports) { m_exports.set(vm, this, exports); }
    // Take ownership of the imported-function bridges (their Strong refs keep the JS callbacks alive for
    // the instance's life; freed here on the owning thread when the instance is collected).
    void adoptImportFuncs(Vector<std::unique_ptr<JsImportFunc>>&& funcs) { m_importFuncs = WTFMove(funcs); }

    DECLARE_INFO;

private:
    JSWasmInstance(VM& vm, Structure* structure, WasmWAMR::Instance* instance)
        : Base(vm, structure), m_instance(instance) { }
    ~JSWasmInstance() { if (m_instance) WasmWAMR::destroy(m_instance); }

    WasmWAMR::Instance* m_instance { nullptr };
    WriteBarrier<JSObject> m_exports;
    WriteBarrier<JSWasmModule> m_module;
    Vector<std::unique_ptr<JsImportFunc>> m_importFuncs;
};

const ClassInfo JSWasmModule::s_info = { "WebAssembly.Module", &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSWasmModule) };
const ClassInfo JSWasmInstance::s_info = { "WebAssembly.Instance", &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSWasmInstance) };

template<typename Visitor>
void JSWasmInstance::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = jsCast<JSWasmInstance*>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_exports);
    visitor.append(thisObject->m_module);
}

DEFINE_VISIT_CHILDREN(JSWasmInstance);

// ---- shared Val <-> JSValue marshalling ---------------------------------
static WasmWAMR::ValKind valKindFromString(const String& s)
{
    if (s == "i64"_s) return WasmWAMR::ValKind::I64;
    if (s == "f32"_s) return WasmWAMR::ValKind::F32;
    if (s == "f64"_s) return WasmWAMR::ValKind::F64;
    return WasmWAMR::ValKind::I32; // "i32" and default
}
static JSValue valToJS(WasmWAMR::Val v)
{
    switch (v.kind) {
    case WasmWAMR::ValKind::I32: return jsNumber(v.u.i32);
    case WasmWAMR::ValKind::I64: return jsNumber(static_cast<double>(v.u.i64)); // 2b: BigInt
    case WasmWAMR::ValKind::F32: return jsNumber(purifyNaN(v.u.f32));
    case WasmWAMR::ValKind::F64: return jsNumber(purifyNaN(v.u.f64));
    default: return jsNumber(0);
    }
}
static WasmWAMR::Val jsToVal(JSGlobalObject* g, JSValue a, WasmWAMR::ValKind kind)
{
    WasmWAMR::Val v;
    v.kind = kind;
    switch (kind) {
    case WasmWAMR::ValKind::I32: v.u.i32 = a.toInt32(g); break;
    case WasmWAMR::ValKind::I64: v.u.i64 = static_cast<int64_t>(a.toNumber(g)); break; // 2b: BigInt
    case WasmWAMR::ValKind::F32: v.u.f32 = static_cast<float>(a.toNumber(g)); break;
    case WasmWAMR::ValKind::F64: v.u.f64 = a.toNumber(g); break;
    default: v.u.i64 = 0; break;
    }
    return v;
}

// ---- WebAssembly.Global -------------------------------------------------
class JSWasmGlobal final : public JSDestructibleObject {
    using Base = JSDestructibleObject;
public:
    template<typename CellType, SubspaceAccess> static CompleteSubspace* subspaceFor(VM& vm) { return &vm.destructibleObjectSpace(); }
    static Structure* createStructure(VM& vm, JSGlobalObject* g, JSValue proto) { return Structure::create(vm, g, proto, TypeInfo(ObjectType, StructureFlags), info()); }

    static JSWasmGlobal* create(VM& vm, JSGlobalObject* g, WasmWAMR::ValKind kind, bool isMutable, WasmWAMR::Val value)
    {
        auto* cell = new (NotNull, allocateCell<JSWasmGlobal>(vm)) JSWasmGlobal(vm, createStructure(vm, g, g->objectPrototype()));
        cell->m_kind = kind; cell->m_mutable = isMutable; cell->m_value = value;
        cell->finishCreation(vm, g);
        return cell;
    }
    static JSWasmGlobal* createBound(VM& vm, JSGlobalObject* g, JSWasmInstance* inst, const String& name, WasmWAMR::ValKind kind, bool isMutable)
    {
        auto* cell = new (NotNull, allocateCell<JSWasmGlobal>(vm)) JSWasmGlobal(vm, createStructure(vm, g, g->objectPrototype()));
        cell->m_kind = kind; cell->m_mutable = isMutable; cell->m_bound = true; cell->m_exportName = name.utf8();
        cell->finishCreation(vm, g);
        cell->m_instance.set(vm, cell, inst);
        return cell;
    }
    static void destroy(JSCell* c) { static_cast<JSWasmGlobal*>(c)->JSWasmGlobal::~JSWasmGlobal(); }
    DECLARE_VISIT_CHILDREN;
    DECLARE_INFO;

    WasmWAMR::ValKind kind() const { return m_kind; }
    bool isMutable() const { return m_mutable; }
    WasmWAMR::Val get()
    {
        if (m_bound && m_instance) {
            WasmWAMR::Val v;
            if (WasmWAMR::exportedGlobalGet(m_instance->instance(), m_exportName.data(), v))
                return v;
        }
        return m_value;
    }
    void set(const WasmWAMR::Val& v)
    {
        if (m_bound && m_instance)
            WasmWAMR::exportedGlobalSet(m_instance->instance(), m_exportName.data(), v);
        m_value = v;
    }
private:
    JSWasmGlobal(VM& vm, Structure* s) : Base(vm, s) { }
    void finishCreation(VM&, JSGlobalObject*);
    WasmWAMR::ValKind m_kind { WasmWAMR::ValKind::I32 };
    bool m_mutable { false };
    bool m_bound { false };
    WasmWAMR::Val m_value;
    CString m_exportName;
    WriteBarrier<JSWasmInstance> m_instance;
};
const ClassInfo JSWasmGlobal::s_info = { "WebAssembly.Global", &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSWasmGlobal) };
template<typename Visitor> void JSWasmGlobal::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = jsCast<JSWasmGlobal*>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_instance);
}
DEFINE_VISIT_CHILDREN(JSWasmGlobal);

static EncodedJSValue JSC_HOST_CALL_ATTRIBUTES wasmGlobalValueGet(JSGlobalObject* g, EncodedJSValue thisValue, PropertyName)
{
    auto* self = jsDynamicCast<JSWasmGlobal*>(g->vm(), JSValue::decode(thisValue));
    if (!self)
        return JSValue::encode(jsUndefined());
    return JSValue::encode(valToJS(self->get()));
}
static bool JSC_HOST_CALL_ATTRIBUTES wasmGlobalValueSet(JSGlobalObject* g, EncodedJSValue thisValue, EncodedJSValue value, PropertyName)
{
    VM& vm = g->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* self = jsDynamicCast<JSWasmGlobal*>(vm, JSValue::decode(thisValue));
    if (!self)
        return false;
    if (!self->isMutable()) {
        throwTypeError(g, scope, "WebAssembly.Global.value: cannot set the value of an immutable global"_s);
        return false;
    }
    self->set(jsToVal(g, JSValue::decode(value), self->kind()));
    return true;
}
JSC_DEFINE_HOST_FUNCTION(wasmGlobalValueOf, (JSGlobalObject* g, CallFrame* callFrame))
{
    auto* self = jsDynamicCast<JSWasmGlobal*>(g->vm(), callFrame->thisValue());
    if (!self)
        return JSValue::encode(jsUndefined());
    return JSValue::encode(valToJS(self->get()));
}
void JSWasmGlobal::finishCreation(VM& vm, JSGlobalObject* g)
{
    Base::finishCreation(vm);
    putDirectCustomAccessor(vm, Identifier::fromString(vm, "value"_s),
        CustomGetterSetter::create(vm, wasmGlobalValueGet, wasmGlobalValueSet),
        static_cast<unsigned>(PropertyAttribute::CustomAccessor));
    putDirectNativeFunction(vm, g, Identifier::fromString(vm, "valueOf"_s), 0, wasmGlobalValueOf, NoIntrinsic, static_cast<unsigned>(PropertyAttribute::DontEnum));
}

// ---- WebAssembly.Memory -------------------------------------------------
class JSWasmMemory final : public JSDestructibleObject {
    using Base = JSDestructibleObject;
public:
    template<typename CellType, SubspaceAccess> static CompleteSubspace* subspaceFor(VM& vm) { return &vm.destructibleObjectSpace(); }
    static Structure* createStructure(VM& vm, JSGlobalObject* g, JSValue proto) { return Structure::create(vm, g, proto, TypeInfo(ObjectType, StructureFlags), info()); }

    // Bound to an instance's default memory (exports, and imports after adoption).
    static JSWasmMemory* createBound(VM& vm, JSGlobalObject* g, JSWasmInstance* inst)
    {
        auto* cell = new (NotNull, allocateCell<JSWasmMemory>(vm)) JSWasmMemory(vm, createStructure(vm, g, g->objectPrototype()));
        cell->finishCreation(vm, g);
        cell->m_instance.set(vm, cell, inst);
        return cell;
    }
    // Standalone (new WebAssembly.Memory({initial,maximum})) — holds its own buffer until (if) it is
    // passed to instantiate() as an import, at which point it is bound to the module's WAMR memory.
    static JSWasmMemory* createStandalone(VM& vm, JSGlobalObject* g, uint32_t initialPages, uint32_t maxPages)
    {
        auto* cell = new (NotNull, allocateCell<JSWasmMemory>(vm)) JSWasmMemory(vm, createStructure(vm, g, g->objectPrototype()));
        cell->m_maxPages = maxPages;
        cell->finishCreation(vm, g);
        auto ab = ArrayBuffer::tryCreate(static_cast<size_t>(initialPages) * 65536, 1);
        if (ab)
            cell->m_cachedBuffer.set(vm, cell, JSArrayBuffer::create(vm, g->arrayBufferStructure(ArrayBufferSharingMode::Default), ab.releaseNonNull()));
        return cell;
    }
    static void destroy(JSCell* c) { static_cast<JSWasmMemory*>(c)->JSWasmMemory::~JSWasmMemory(); }
    DECLARE_VISIT_CHILDREN;
    DECLARE_INFO;

    void bindToInstance(VM& vm, JSWasmInstance* inst) { m_instance.set(vm, this, inst); m_cachedBuffer.clear(); }

    JSValue buffer(VM& vm, JSGlobalObject* g)
    {
        if (m_instance) {
            size_t len = 0;
            uint8_t* base = WasmWAMR::memoryData(m_instance->instance(), len);
            if (!base)
                return jsUndefined();
            if (JSArrayBuffer* cached = m_cachedBuffer.get()) {
                if (cached->impl() && cached->impl()->data() == base && cached->impl()->byteLength() == len)
                    return cached;
            }
            auto destructor = createSharedTask<void(void*)>([](void*) { }); // WAMR owns the memory
            auto ab = ArrayBuffer::createFromBytes(base, len, WTFMove(destructor));
            JSArrayBuffer* jsBuffer = JSArrayBuffer::create(vm, g->arrayBufferStructure(ArrayBufferSharingMode::Default), WTFMove(ab));
            m_cachedBuffer.set(vm, this, jsBuffer);
            return jsBuffer;
        }
        return m_cachedBuffer.get() ? JSValue(m_cachedBuffer.get()) : jsUndefined();
    }
    // Returns previous page count, or -1 on failure.
    int grow(VM&, uint32_t deltaPages)
    {
        if (m_instance) {
            int old = WasmWAMR::memoryGrow(m_instance->instance(), deltaPages);
            if (old >= 0)
                m_cachedBuffer.clear(); // next .buffer rebuilds over the (possibly relocated) base
            return old;
        }
        return -1; // standalone-only memory can't grow without a backing instance
    }
private:
    JSWasmMemory(VM& vm, Structure* s) : Base(vm, s) { }
    void finishCreation(VM&, JSGlobalObject*);
    WriteBarrier<JSWasmInstance> m_instance;
    WriteBarrier<JSArrayBuffer> m_cachedBuffer;
    uint32_t m_maxPages { 0 };
};
const ClassInfo JSWasmMemory::s_info = { "WebAssembly.Memory", &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSWasmMemory) };
template<typename Visitor> void JSWasmMemory::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = jsCast<JSWasmMemory*>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_instance);
    visitor.append(thisObject->m_cachedBuffer);
}
DEFINE_VISIT_CHILDREN(JSWasmMemory);

static EncodedJSValue JSC_HOST_CALL_ATTRIBUTES wasmMemoryBufferGet(JSGlobalObject* g, EncodedJSValue thisValue, PropertyName)
{
    auto* self = jsDynamicCast<JSWasmMemory*>(g->vm(), JSValue::decode(thisValue));
    if (!self)
        return JSValue::encode(jsUndefined());
    return JSValue::encode(self->buffer(g->vm(), g));
}
JSC_DEFINE_HOST_FUNCTION(wasmMemoryGrow, (JSGlobalObject* g, CallFrame* callFrame))
{
    VM& vm = g->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* self = jsDynamicCast<JSWasmMemory*>(vm, callFrame->thisValue());
    if (!self)
        return throwVMTypeError(g, scope, "WebAssembly.Memory.grow: not a Memory"_s);
    uint32_t delta = callFrame->argument(0).toUInt32(g);
    RETURN_IF_EXCEPTION(scope, { });
    int old = self->grow(vm, delta);
    if (old < 0)
        return throwVMRangeError(g, scope, "WebAssembly.Memory.grow failed"_s);
    return JSValue::encode(jsNumber(old));
}
void JSWasmMemory::finishCreation(VM& vm, JSGlobalObject* g)
{
    Base::finishCreation(vm);
    putDirectCustomAccessor(vm, Identifier::fromString(vm, "buffer"_s),
        CustomGetterSetter::create(vm, wasmMemoryBufferGet, nullptr),
        static_cast<unsigned>(PropertyAttribute::CustomAccessor | PropertyAttribute::ReadOnly));
    putDirectNativeFunction(vm, g, Identifier::fromString(vm, "grow"_s), 1, wasmMemoryGrow, NoIntrinsic, static_cast<unsigned>(PropertyAttribute::DontEnum));
}

// ---- WebAssembly.Table --------------------------------------------------
class JSWasmTable final : public JSDestructibleObject {
    using Base = JSDestructibleObject;
public:
    template<typename CellType, SubspaceAccess> static CompleteSubspace* subspaceFor(VM& vm) { return &vm.destructibleObjectSpace(); }
    static Structure* createStructure(VM& vm, JSGlobalObject* g, JSValue proto) { return Structure::create(vm, g, proto, TypeInfo(ObjectType, StructureFlags), info()); }

    static JSWasmTable* createBound(VM& vm, JSGlobalObject* g, JSWasmInstance* inst, const String& name)
    {
        auto* cell = new (NotNull, allocateCell<JSWasmTable>(vm)) JSWasmTable(vm, createStructure(vm, g, g->objectPrototype()));
        cell->m_bound = true; cell->m_exportName = name.utf8();
        cell->finishCreation(vm, g);
        cell->m_instance.set(vm, cell, inst);
        return cell;
    }
    static JSWasmTable* createStandalone(VM& vm, JSGlobalObject* g, uint32_t initial, uint32_t maxSize)
    {
        auto* cell = new (NotNull, allocateCell<JSWasmTable>(vm)) JSWasmTable(vm, createStructure(vm, g, g->objectPrototype()));
        cell->m_max = maxSize;
        cell->finishCreation(vm, g);
        cell->m_elements.set(vm, cell, constructEmptyArray(g, nullptr, initial));
        return cell;
    }
    static void destroy(JSCell* c) { static_cast<JSWasmTable*>(c)->JSWasmTable::~JSWasmTable(); }
    DECLARE_VISIT_CHILDREN;
    DECLARE_INFO;

    uint32_t length()
    {
        if (m_bound && m_instance) {
            uint32_t cur = 0, mx = 0;
            if (WasmWAMR::exportedTableInfo(m_instance->instance(), m_exportName.data(), cur, mx))
                return cur;
        }
        return m_elements ? m_elements->length() : 0;
    }
    JSArray* elements() const { return m_elements.get(); }
private:
    JSWasmTable(VM& vm, Structure* s) : Base(vm, s) { }
    void finishCreation(VM&, JSGlobalObject*);
    bool m_bound { false };
    CString m_exportName;
    uint32_t m_max { 0 };
    WriteBarrier<JSWasmInstance> m_instance;
    WriteBarrier<JSArray> m_elements; // JS-side element store (standalone tables + funcref set())
};
const ClassInfo JSWasmTable::s_info = { "WebAssembly.Table", &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSWasmTable) };
template<typename Visitor> void JSWasmTable::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = jsCast<JSWasmTable*>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_instance);
    visitor.append(thisObject->m_elements);
}
DEFINE_VISIT_CHILDREN(JSWasmTable);

static EncodedJSValue JSC_HOST_CALL_ATTRIBUTES wasmTableLengthGet(JSGlobalObject* g, EncodedJSValue thisValue, PropertyName)
{
    auto* self = jsDynamicCast<JSWasmTable*>(g->vm(), JSValue::decode(thisValue));
    return JSValue::encode(jsNumber(self ? self->length() : 0));
}
JSC_DEFINE_HOST_FUNCTION(wasmTableGet, (JSGlobalObject* g, CallFrame* callFrame))
{
    VM& vm = g->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* self = jsDynamicCast<JSWasmTable*>(vm, callFrame->thisValue());
    if (!self)
        return throwVMTypeError(g, scope, "WebAssembly.Table.get: not a Table"_s);
    uint32_t idx = callFrame->argument(0).toUInt32(g);
    RETURN_IF_EXCEPTION(scope, { });
    // JS-side element store holds funcrefs written via set(); WAMR-backed slots return null (funcref<->JS
    // identity round-tripping is a follow-up). Spec-shaped: out-of-range throws RangeError.
    if (idx >= self->length())
        return throwVMRangeError(g, scope, "WebAssembly.Table.get: index out of bounds"_s);
    if (JSArray* els = self->elements()) {
        if (idx < els->length())
            return JSValue::encode(els->getIndex(g, idx));
    }
    return JSValue::encode(jsNull());
}
JSC_DEFINE_HOST_FUNCTION(wasmTableSet, (JSGlobalObject* g, CallFrame* callFrame))
{
    VM& vm = g->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* self = jsDynamicCast<JSWasmTable*>(vm, callFrame->thisValue());
    if (!self)
        return throwVMTypeError(g, scope, "WebAssembly.Table.set: not a Table"_s);
    uint32_t idx = callFrame->argument(0).toUInt32(g);
    RETURN_IF_EXCEPTION(scope, { });
    if (idx >= self->length())
        return throwVMRangeError(g, scope, "WebAssembly.Table.set: index out of bounds"_s);
    if (JSArray* els = self->elements())
        els->putDirectIndex(g, idx, callFrame->argument(1));
    return JSValue::encode(jsUndefined());
}
JSC_DEFINE_HOST_FUNCTION(wasmTableGrow, (JSGlobalObject* g, CallFrame* callFrame))
{
    VM& vm = g->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* self = jsDynamicCast<JSWasmTable*>(vm, callFrame->thisValue());
    if (!self)
        return throwVMTypeError(g, scope, "WebAssembly.Table.grow: not a Table"_s);
    uint32_t oldLen = self->length();
    uint32_t delta = callFrame->argument(0).toUInt32(g);
    RETURN_IF_EXCEPTION(scope, { });
    if (JSArray* els = self->elements()) {
        for (uint32_t i = 0; i < delta; ++i)
            els->push(g, callFrame->argument(1));
    }
    return JSValue::encode(jsNumber(oldLen));
}
void JSWasmTable::finishCreation(VM& vm, JSGlobalObject* g)
{
    Base::finishCreation(vm);
    putDirectCustomAccessor(vm, Identifier::fromString(vm, "length"_s),
        CustomGetterSetter::create(vm, wasmTableLengthGet, nullptr),
        static_cast<unsigned>(PropertyAttribute::CustomAccessor | PropertyAttribute::ReadOnly));
    putDirectNativeFunction(vm, g, Identifier::fromString(vm, "get"_s), 1, wasmTableGet, NoIntrinsic, static_cast<unsigned>(PropertyAttribute::DontEnum));
    putDirectNativeFunction(vm, g, Identifier::fromString(vm, "set"_s), 2, wasmTableSet, NoIntrinsic, static_cast<unsigned>(PropertyAttribute::DontEnum));
    putDirectNativeFunction(vm, g, Identifier::fromString(vm, "grow"_s), 1, wasmTableGrow, NoIntrinsic, static_cast<unsigned>(PropertyAttribute::DontEnum));
}

// ---- helpers ------------------------------------------------------------

static std::optional<Vector<uint8_t>> readBufferSource(JSGlobalObject* globalObject, VM& vm, JSValue value, ThrowScope& scope)
{
    JSObject* object = value.getObject();
    if (object) {
        if (auto* buffer = jsDynamicCast<JSArrayBuffer*>(vm, object)) {
            ArrayBuffer* impl = buffer->impl();
            Vector<uint8_t> out;
            out.append(static_cast<const uint8_t*>(impl->data()), impl->byteLength());
            return out;
        }
        if (auto* view = jsDynamicCast<JSArrayBufferView*>(vm, object)) {
            // JSArrayBufferView::byteLength() is out-of-line and not DLL-exported from JSC.
            // Go through the exported possiblySharedImpl() to the WTF ArrayBufferView, whose
            // data()/byteLength() are inline — and which correctly accounts for byteOffset.
            if (RefPtr<ArrayBufferView> impl = view->possiblySharedImpl()) {
                Vector<uint8_t> out;
                out.append(static_cast<const uint8_t*>(impl->data()), impl->byteLength());
                return out;
            }
            return std::nullopt;
        }
    }
    throwTypeError(globalObject, scope, "WebAssembly: first argument must be an ArrayBuffer or ArrayBufferView"_s);
    return std::nullopt;
}

// Build a callable JS function for an exported wasm function.
static JSValue makeExportedFunction(VM& vm, JSGlobalObject* globalObject, JSWasmInstance* instanceObject, const String& exportName)
{
    WasmWAMR::Instance* instance = instanceObject->instance();
    CString nameUtf8 = exportName.utf8();

    auto fn = JSNativeStdFunction::create(vm, globalObject, 0, exportName,
        [instance, nameUtf8](JSGlobalObject* globalObject, CallFrame* callFrame) -> EncodedJSValue {
            VM& vm = globalObject->vm();
            auto scope = DECLARE_THROW_SCOPE(vm);

            Vector<WasmWAMR::ValKind> params, results;
            if (!WasmWAMR::functionSignature(instance, nameUtf8.data(), params, results))
                return JSValue::encode(throwTypeError(globalObject, scope, "WebAssembly: not an exported function"_s));

            Vector<WasmWAMR::Val> args(params.size());
            for (size_t i = 0; i < params.size(); ++i) {
                JSValue a = callFrame->argument(i);
                WasmWAMR::Val& v = args[i];
                v.kind = params[i];
                switch (params[i]) {
                case WasmWAMR::ValKind::I32: v.u.i32 = a.toInt32(globalObject); break;
                case WasmWAMR::ValKind::I64: v.u.i64 = static_cast<int64_t>(a.toNumber(globalObject)); break; // 2b: BigInt
                case WasmWAMR::ValKind::F32: v.u.f32 = static_cast<float>(a.toNumber(globalObject)); break;
                case WasmWAMR::ValKind::F64: v.u.f64 = a.toNumber(globalObject); break;
                default: v.kind = WasmWAMR::ValKind::I32; v.u.i32 = 0; break;
                }
                RETURN_IF_EXCEPTION(scope, { });
            }

            Vector<WasmWAMR::Val> rets(results.size());
            String error;
            if (!WasmWAMR::callFunction(instance, nameUtf8.data(), args.data(), args.size(), rets.data(), rets.size(), error))
                return JSValue::encode(throwTypeError(globalObject, scope, makeString("WebAssembly runtime error: "_s, error)));

            auto toJS = [&](const WasmWAMR::Val& v) -> JSValue {
                switch (v.kind) {
                case WasmWAMR::ValKind::I32: return jsNumber(v.u.i32);
                case WasmWAMR::ValKind::I64: return jsNumber(static_cast<double>(v.u.i64)); // 2b: BigInt
                case WasmWAMR::ValKind::F32: return jsNumber(purifyNaN(v.u.f32));
                case WasmWAMR::ValKind::F64: return jsNumber(purifyNaN(v.u.f64));
                default: return jsUndefined();
                }
            };
            if (results.isEmpty())
                return JSValue::encode(jsUndefined());
            if (results.size() == 1)
                return JSValue::encode(toJS(rets[0]));
            JSArray* array = constructEmptyArray(globalObject, nullptr, results.size());
            RETURN_IF_EXCEPTION(scope, { });
            for (size_t i = 0; i < rets.size(); ++i)
                array->putDirectIndex(globalObject, i, toJS(rets[i]));
            return JSValue::encode(array);
        });

    // GC edge: keep the instance alive as long as any exported function is held.
    fn->putDirect(vm, Identifier::fromString(vm, "__wasmInstanceRef__"_s), instanceObject,
        static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly));
    return fn;
}

// Wrap an exported memory as a real WebAssembly.Memory bound to the instance's linear memory.
static JSValue makeExportedMemory(VM& vm, JSGlobalObject* globalObject, JSWasmInstance* instanceObject)
{
    size_t byteLength = 0;
    if (!WasmWAMR::memoryData(instanceObject->instance(), byteLength))
        return jsUndefined();
    return JSWasmMemory::createBound(vm, globalObject, instanceObject);
}

// ---- top-level API ------------------------------------------------------

JSC_DEFINE_HOST_FUNCTION(validateImpl, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto bytes = readBufferSource(globalObject, vm, callFrame->argument(0), scope);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(WasmWAMR::validate(bytes->data(), bytes->size())));
}

// Revenant diag (CF Turnstile loop): does the challenge's proof-of-work WASM hit our import wall?
extern "C" void WebCoreBrowserLog(const char*);

static JSWasmInstance* instantiateModule(VM& vm, JSGlobalObject* globalObject, JSWasmModule* moduleObject, JSValue importObject, ThrowScope& scope)
{
    WasmWAMR::Module* module = moduleObject->module();
    unsigned nImports = WasmWAMR::importCount(module);
    String error;
    WasmWAMR::Instance* instance = nullptr;
    Vector<std::unique_ptr<JsImportFunc>> ownedFuncs;
    JSWasmMemory* importedMemory = nullptr; // a passed WebAssembly.Memory, bound to WAMR's mem post-instantiate

    if (!nImports)
        instance = WasmWAMR::instantiate(module, error);
    else {
        JSObject* impObj = importObject.isObject() ? asObject(importObject) : nullptr;
        if (!impObj) {
            throwTypeError(globalObject, scope, "WebAssembly.instantiate: an import object is required for a module with imports"_s);
            return nullptr;
        }
        Vector<WasmWAMR::HostImport> hostImports;
        hostImports.reserveCapacity(nImports);
        for (unsigned i = 0; i < nImports; ++i) {
            String modName, fieldName;
            WasmWAMR::ExternKind kind;
            WasmWAMR::importInfo(module, i, modName, fieldName, kind);

            WasmWAMR::HostImport hi;
            hi.kind = kind;
            hi.moduleName = modName;
            hi.fieldName = fieldName;

            // Diag: dump exactly what the module imports so we know if func-only bridging suffices for a
            // given site (WAMR auto-provisions memory/global/table; only funcs need host resolution).
            { auto b = makeString("wasmdiag: import[", i, "] ", modName, ".", fieldName, " kind=",
                kind == WasmWAMR::ExternKind::Func ? "func" : kind == WasmWAMR::ExternKind::Memory ? "memory"
                : kind == WasmWAMR::ExternKind::Global ? "global" : kind == WasmWAMR::ExternKind::Table ? "table" : "?");
              WebCoreBrowserLog(b.utf8().data()); }

            JSValue nsVal = impObj->get(globalObject, Identifier::fromString(vm, modName));
            RETURN_IF_EXCEPTION(scope, nullptr);
            JSValue fieldVal = nsVal.isObject() ? asObject(nsVal)->get(globalObject, Identifier::fromString(vm, fieldName)) : jsUndefined();
            RETURN_IF_EXCEPTION(scope, nullptr);

            if (kind == WasmWAMR::ExternKind::Func) {
                if (!fieldVal.isCallable(vm)) {
                    throwTypeError(globalObject, scope, makeString("WebAssembly.instantiate: import "_s, modName, '.', fieldName, " is not a function"_s));
                    return nullptr;
                }
                WasmWAMR::importFuncSignature(module, i, hi.funcParams, hi.funcResults);
                auto jf = makeUnique<JsImportFunc>();
                jf->globalObject = globalObject;
                jf->function.set(vm, asObject(fieldVal));
                jf->params = hi.funcParams;
                jf->results = hi.funcResults;
                hi.funcCtx = jf.get();
                ownedFuncs.append(WTFMove(jf));
            }
            else if (kind == WasmWAMR::ExternKind::Memory) {
                // WAMR auto-allocates the module's memory; if the host passed a WebAssembly.Memory, bind it
                // to that memory after instantiate so its .buffer aliases the WASM linear memory (the
                // read-.buffer-after-instantiate contract emscripten relies on).
                if (auto* m = jsDynamicCast<JSWasmMemory*>(vm, fieldVal))
                    importedMemory = m;
            }
            // Global/Table imports: WAMR auto-provisions from the module's declared init/limits. (Passing a
            // specific host global VALUE to an imported-but-not-exported global has no WAMR public API and
            // is a known follow-up; declared-init covers the common emscripten cases.)
            hostImports.append(WTFMove(hi));
        }
        WasmWAMR::setJsImportCallHook(jsImportCallHook); // idempotent
        instance = WasmWAMR::instantiateWithImports(module, WTFMove(hostImports), error);
    }

    if (!instance) {
        throwTypeError(globalObject, scope, makeString("WebAssembly link error: "_s, error));
        return nullptr;
    }
    JSWasmInstance* instanceObject = JSWasmInstance::create(vm, globalObject, moduleObject, instance);
    instanceObject->adoptImportFuncs(WTFMove(ownedFuncs));
    if (importedMemory)
        importedMemory->bindToInstance(vm, instanceObject); // .buffer now aliases WAMR's linear memory

    // Build exports from the module's export list.
    JSObject* exports = constructEmptyObject(globalObject);
    unsigned count = WasmWAMR::exportCount(module);
    for (unsigned i = 0; i < count; ++i) {
        String name;
        WasmWAMR::ExternKind kind;
        if (!WasmWAMR::exportInfo(module, i, name, kind))
            continue;
        JSValue value;
        switch (kind) {
        case WasmWAMR::ExternKind::Func:
            value = makeExportedFunction(vm, globalObject, instanceObject, name);
            break;
        case WasmWAMR::ExternKind::Memory:
            value = makeExportedMemory(vm, globalObject, instanceObject);
            break;
        case WasmWAMR::ExternKind::Global: {
            WasmWAMR::ValKind gk; bool gm = false;
            if (WasmWAMR::exportedGlobalType(instanceObject->instance(), name.utf8().data(), gk, gm))
                value = JSWasmGlobal::createBound(vm, globalObject, instanceObject, name, gk, gm);
            break;
        }
        case WasmWAMR::ExternKind::Table:
            value = JSWasmTable::createBound(vm, globalObject, instanceObject, name);
            break;
        default:
            continue;
        }
        if (value)
            exports->putDirect(vm, Identifier::fromString(vm, name), value, static_cast<unsigned>(PropertyAttribute::ReadOnly));
    }
    instanceObject->setExports(vm, exports);
    instanceObject->putDirect(vm, Identifier::fromString(vm, "exports"_s), exports, static_cast<unsigned>(PropertyAttribute::ReadOnly));
    return instanceObject;
}

JSC_DEFINE_HOST_FUNCTION(compileImpl, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto bytes = readBufferSource(globalObject, vm, callFrame->argument(0), scope);
    if (scope.exception()) {
        scope.clearException();
        return JSValue::encode(JSPromise::rejectedPromise(globalObject, createTypeError(globalObject, "WebAssembly.compile: expected BufferSource"_s)));
    }
    String error;
    WasmWAMR::Module* module = WasmWAMR::compile(bytes->data(), bytes->size(), error);
    { char b[96]; snprintf(b, sizeof b, "wasmdiag: compile bytes=%u -> %s", static_cast<unsigned>(bytes->size()), module ? "ok" : "FAIL"); WebCoreBrowserLog(b); }
    if (!module)
        return JSValue::encode(JSPromise::rejectedPromise(globalObject, createTypeError(globalObject, makeString("WebAssembly.compile: "_s, error))));
    JSWasmModule* moduleObject = JSWasmModule::create(vm, globalObject, module);
    return JSValue::encode(JSPromise::resolvedPromise(globalObject, moduleObject));
}

JSC_DEFINE_HOST_FUNCTION(instantiateImpl, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue first = callFrame->argument(0);

    // Overload 1: instantiate(module, importObject) — first arg is a compiled Module → resolves Instance.
    if (auto* moduleObject = jsDynamicCast<JSWasmModule*>(vm, first)) {
        JSWasmInstance* instanceObject = instantiateModule(vm, globalObject, moduleObject, callFrame->argument(1), scope);
        if (scope.exception()) {
            JSValue e = scope.exception()->value();
            scope.clearException();
            return JSValue::encode(JSPromise::rejectedPromise(globalObject, e));
        }
        return JSValue::encode(JSPromise::resolvedPromise(globalObject, instanceObject));
    }

    // Overload 2: instantiate(bytes) — compile + instantiate → resolves { module, instance }.
    auto bytes = readBufferSource(globalObject, vm, first, scope);
    if (scope.exception()) {
        scope.clearException();
        return JSValue::encode(JSPromise::rejectedPromise(globalObject, createTypeError(globalObject, "WebAssembly.instantiate: expected BufferSource or Module"_s)));
    }
    String error;
    WasmWAMR::Module* module = WasmWAMR::compile(bytes->data(), bytes->size(), error);
    if (!module)
        return JSValue::encode(JSPromise::rejectedPromise(globalObject, createTypeError(globalObject, makeString("WebAssembly.instantiate: "_s, error))));
    JSWasmModule* moduleObject = JSWasmModule::create(vm, globalObject, module);
    JSWasmInstance* instanceObject = instantiateModule(vm, globalObject, moduleObject, callFrame->argument(1), scope);
    if (scope.exception()) {
        JSValue e = scope.exception()->value();
        scope.clearException();
        return JSValue::encode(JSPromise::rejectedPromise(globalObject, e));
    }
    JSObject* result = constructEmptyObject(globalObject);
    result->putDirect(vm, Identifier::fromString(vm, "module"_s), moduleObject, 0);
    result->putDirect(vm, Identifier::fromString(vm, "instance"_s), instanceObject, 0);
    return JSValue::encode(JSPromise::resolvedPromise(globalObject, result));
}

// ---- install ------------------------------------------------------------

// ---- WebAssembly.Global / Memory / Table constructors -------------------
JSC_DEFINE_HOST_FUNCTION(constructGlobal, (JSGlobalObject* g, CallFrame* cf))
{
    VM& vm = g->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue d = cf->argument(0);
    if (!d.isObject())
        return throwVMTypeError(g, scope, "WebAssembly.Global: descriptor must be an object"_s);
    JSObject* desc = asObject(d);
    JSValue vtV = desc->get(g, Identifier::fromString(vm, "value"_s));
    RETURN_IF_EXCEPTION(scope, { });
    String vt = vtV.toWTFString(g);
    RETURN_IF_EXCEPTION(scope, { });
    WasmWAMR::ValKind kind = valKindFromString(vt);
    JSValue mV = desc->get(g, Identifier::fromString(vm, "mutable"_s));
    RETURN_IF_EXCEPTION(scope, { });
    bool isMutable = mV.toBoolean(g);
    WasmWAMR::Val v = jsToVal(g, cf->argument(1), kind);
    return JSValue::encode(JSWasmGlobal::create(vm, g, kind, isMutable, v));
}
JSC_DEFINE_HOST_FUNCTION(constructMemory, (JSGlobalObject* g, CallFrame* cf))
{
    VM& vm = g->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue d = cf->argument(0);
    if (!d.isObject())
        return throwVMTypeError(g, scope, "WebAssembly.Memory: descriptor must be an object"_s);
    JSObject* desc = asObject(d);
    JSValue initV = desc->get(g, Identifier::fromString(vm, "initial"_s));
    RETURN_IF_EXCEPTION(scope, { });
    uint32_t initial = initV.toUInt32(g);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue maxV = desc->get(g, Identifier::fromString(vm, "maximum"_s));
    RETURN_IF_EXCEPTION(scope, { });
    uint32_t maxPages = maxV.isUndefined() ? 0 : maxV.toUInt32(g);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(JSWasmMemory::createStandalone(vm, g, initial, maxPages));
}
JSC_DEFINE_HOST_FUNCTION(constructTable, (JSGlobalObject* g, CallFrame* cf))
{
    VM& vm = g->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue d = cf->argument(0);
    if (!d.isObject())
        return throwVMTypeError(g, scope, "WebAssembly.Table: descriptor must be an object"_s);
    JSObject* desc = asObject(d);
    JSValue initV = desc->get(g, Identifier::fromString(vm, "initial"_s));
    RETURN_IF_EXCEPTION(scope, { });
    uint32_t initial = initV.toUInt32(g);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue maxV = desc->get(g, Identifier::fromString(vm, "maximum"_s));
    RETURN_IF_EXCEPTION(scope, { });
    uint32_t maxSize = maxV.isUndefined() ? 0 : maxV.toUInt32(g);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(JSWasmTable::createStandalone(vm, g, initial, maxSize));
}

void installWebAssemblyWAMR(JSC::JSGlobalObject& globalObject)
{
    VM& vm = globalObject.vm();
    JSObject* webAssembly = constructEmptyObject(&globalObject);

    auto addFunction = [&](ASCIILiteral name, unsigned length, NativeFunction fn) {
        Identifier ident = Identifier::fromString(vm, name);
        webAssembly->putDirect(vm, ident, JSFunction::create(vm, &globalObject, length, name, fn), static_cast<unsigned>(PropertyAttribute::DontEnum));
    };
    addFunction("validate"_s, 1, validateImpl);
    addFunction("compile"_s, 1, compileImpl);
    addFunction("instantiate"_s, 1, instantiateImpl);

    // Constructors: created as functions usable with `new` (same NativeFunction for call + construct).
    auto addConstructor = [&](ASCIILiteral name, unsigned length, NativeFunction fn) {
        Identifier ident = Identifier::fromString(vm, name);
        webAssembly->putDirect(vm, ident, JSFunction::create(vm, &globalObject, length, name, fn, NoIntrinsic, fn), static_cast<unsigned>(PropertyAttribute::DontEnum));
    };
    addConstructor("Global"_s, 1, constructGlobal);
    addConstructor("Memory"_s, 1, constructMemory);
    addConstructor("Table"_s, 1, constructTable);

    globalObject.putDirect(vm, Identifier::fromString(vm, "WebAssembly"_s), webAssembly, static_cast<unsigned>(PropertyAttribute::DontEnum));
}

} // namespace WebCore
