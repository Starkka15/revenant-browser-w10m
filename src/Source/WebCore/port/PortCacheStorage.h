// PortCacheStorage — real, in-process CacheStorage (window.caches / Cache API) backing for
// the W10M/UWP render driver. WebKit2 normally brokers Cache API calls over IPC to the
// network process (WebCacheStorageConnection -> CacheStorageEngine); this single-process
// port implements a functional CacheStorageConnection directly, backed by an in-memory
// store (per-ClientOrigin: caches; per-cache: a list of Records holding request/response/
// response-body). The EmptyClients default (DummyCacheStorageConnection) is a no-op that
// never even invokes its callbacks, so every caches.* promise hangs forever. This one
// actually stores and retrieves, honoring the DOMCacheEngine callback shapes.
//
// In-memory only for v1 (records are lost on app exit), matching the DOM-storage / cookie
// port. Persistence is a later TODO.
#pragma once

#include <wtf/Ref.h>

namespace WebCore {
class CacheStorageProvider;
}

namespace WebCorePort {

// Returns a provider whose createCacheStorageConnection() hands back a single, shared,
// in-memory PortCacheStorageConnection (so caches survive across same-page navigations for
// the lifetime of the provider). Install as PageConfiguration::cacheStorageProvider.
WTF::Ref<WebCore::CacheStorageProvider> createPortCacheStorageProvider();

}
