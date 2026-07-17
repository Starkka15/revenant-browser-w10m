/*
 * Revenant: installs a real window.WebAssembly backed by WAMR (see WasmWAMR.h),
 * because JSC's own WASM engine is 64-bit-only and disabled on ARM32. Called from
 * JSDOMWindowBase::finishCreation.
 */
#pragma once

namespace JSC {
class JSGlobalObject;
}

namespace WebCore {

// Installs the "WebAssembly" namespace object on the given global object.
void installWebAssemblyWAMR(JSC::JSGlobalObject&);

} // namespace WebCore
