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
#include <JavaScriptCore/ArrayBuffer.h>
#include <JavaScriptCore/ArrayBufferView.h>
#include <JavaScriptCore/CatchScope.h>
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

    DECLARE_INFO;

private:
    JSWasmInstance(VM& vm, Structure* structure, WasmWAMR::Instance* instance)
        : Base(vm, structure), m_instance(instance) { }
    ~JSWasmInstance() { if (m_instance) WasmWAMR::destroy(m_instance); }

    WasmWAMR::Instance* m_instance { nullptr };
    WriteBarrier<JSObject> m_exports;
    WriteBarrier<JSWasmModule> m_module;
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

// Wrap an exported memory as a minimal { buffer } object (2a). Full WebAssembly.Memory = 2b.
static JSValue makeExportedMemory(VM& vm, JSGlobalObject* globalObject, JSWasmInstance* instanceObject)
{
    size_t byteLength = 0;
    uint8_t* base = WasmWAMR::memoryData(instanceObject->instance(), byteLength);
    if (!base)
        return jsUndefined();
    // WAMR owns the linear memory; ArrayBuffer must not free it.
    auto destructor = createSharedTask<void(void*)>([](void*) { });
    auto arrayBuffer = ArrayBuffer::createFromBytes(base, byteLength, WTFMove(destructor));
    JSArrayBuffer* jsBuffer = JSArrayBuffer::create(vm, globalObject->arrayBufferStructure(ArrayBufferSharingMode::Default), WTFMove(arrayBuffer));

    JSObject* memory = constructEmptyObject(globalObject);
    memory->putDirect(vm, Identifier::fromString(vm, "buffer"_s), jsBuffer, static_cast<unsigned>(PropertyAttribute::ReadOnly));
    return memory;
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

static JSWasmInstance* instantiateModule(VM& vm, JSGlobalObject* globalObject, JSWasmModule* moduleObject, ThrowScope& scope)
{
    WasmWAMR::Module* module = moduleObject->module();
    if (WasmWAMR::hasImports(module)) {
        throwTypeError(globalObject, scope, "WebAssembly: modules with imports are not yet supported (2a)"_s);
        return nullptr;
    }
    String error;
    WasmWAMR::Instance* instance = WasmWAMR::instantiate(module, error);
    if (!instance) {
        throwTypeError(globalObject, scope, makeString("WebAssembly link error: "_s, error));
        return nullptr;
    }
    JSWasmInstance* instanceObject = JSWasmInstance::create(vm, globalObject, moduleObject, instance);

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
        default:
            continue; // Table/Global exports = 2b
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

    // Overload 1: instantiate(module) — first arg is a compiled Module → resolves Instance.
    if (auto* moduleObject = jsDynamicCast<JSWasmModule*>(vm, first)) {
        JSWasmInstance* instanceObject = instantiateModule(vm, globalObject, moduleObject, scope);
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
    JSWasmInstance* instanceObject = instantiateModule(vm, globalObject, moduleObject, scope);
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

    globalObject.putDirect(vm, Identifier::fromString(vm, "WebAssembly"_s), webAssembly, static_cast<unsigned>(PropertyAttribute::DontEnum));
}

} // namespace WebCore
