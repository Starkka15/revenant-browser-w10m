// PortStorageProvider — real in-memory DOM storage (localStorage/sessionStorage) and a
// cookie StorageSessionProvider for the W10M render driver. The EmptyClients versions are
// no-ops (StorageArea drops all writes; StorageSessionProvider returns nullptr), which
// makes modern SPAs stall silently. These are functional, spec-correct backends.
#pragma once
#include <wtf/Ref.h>

namespace WebCore {
class StorageNamespaceProvider;
class StorageSessionProvider;
}

namespace WebCorePort {
WTF::Ref<WebCore::StorageNamespaceProvider> createPortStorageNamespaceProvider();
WTF::Ref<WebCore::StorageSessionProvider> createPortStorageSessionProvider();
}
