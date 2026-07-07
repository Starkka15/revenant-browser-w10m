#include "config.h"
#include "PortCacheStorage.h"

#include "CacheQueryOptions.h"
#include "CacheStorageProvider.h"
#include "ClientOrigin.h"
#include "DOMCacheEngine.h"
#include "ResourceRequest.h"
#include "RetrieveRecordsOptions.h"
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace WebCorePort {
using namespace WebCore;
using namespace WebCore::DOMCacheEngine;

// One named cache: an ordered list of records + a per-cache record-id counter.
struct PortCacheEntry {
    WTF_MAKE_STRUCT_FAST_ALLOCATED;
    uint64_t identifier { 0 };
    String name;
    ClientOrigin origin;
    uint64_t nextRecordIdentifier { 0 };
    Vector<Record> records;
};

// The set of caches for one origin, plus the update counter DOMCacheStorage uses to know
// when its cached name list is stale (bumped on every create/remove).
struct PortOriginCaches {
    uint64_t updateCounter { 0 };
    Vector<uint64_t> cacheIdentifiers; // creation order; keys into m_caches
};

// The shared in-memory store. ThreadSafeRefCounted because CacheStorageConnection is (a
// worker may hold a reference); guarded by a lock so worker + main threads are safe.
class PortCacheStore : public ThreadSafeRefCounted<PortCacheStore> {
public:
    static Ref<PortCacheStore> create() { return adoptRef(*new PortCacheStore); }

    void open(const ClientOrigin& origin, const String& name, CacheIdentifierCallback&& callback)
    {
        // Compute under the lock, then RELEASE the lock before invoking the callback. The callback
        // can re-enter the store synchronously (WebCore's caches.open() calls retrieveCaches() then
        // open() from within retrieveCaches's completion) — holding m_lock across the callback
        // self-deadlocks on the non-recursive Lock (froze YouTube: it calls caches.open()).
        auto result = [&]() -> CacheIdentifierOperationResult {
            Locker locker { m_lock };
            // Isolate the key: it may be inserted from a worker thread and read from main.
            auto& originCaches = m_originCaches.ensure(origin.isolatedCopy(), [] { return PortOriginCaches { }; }).iterator->value;
            for (auto id : originCaches.cacheIdentifiers) {
                auto it = m_caches.find(id);
                if (it != m_caches.end() && it->value->name == name)
                    return CacheIdentifierOperationResult { id, false };
            }
            auto entry = makeUnique<PortCacheEntry>();
            uint64_t id = ++m_nextCacheIdentifier;
            entry->identifier = id;
            entry->name = name.isolatedCopy();
            entry->origin = origin.isolatedCopy();
            m_caches.set(id, WTFMove(entry));
            originCaches.cacheIdentifiers.append(id);
            ++originCaches.updateCounter;
            return CacheIdentifierOperationResult { id, false };
        }();
        callback(result);
    }

    void remove(uint64_t cacheIdentifier, CacheIdentifierCallback&& callback)
    {
        auto result = [&]() -> CacheIdentifierOperationResult {
            Locker locker { m_lock };
            auto it = m_caches.find(cacheIdentifier);
            if (it == m_caches.end())
                return CacheIdentifierOperationResult { 0, false };
            auto originIt = m_originCaches.find(it->value->origin);
            if (originIt != m_originCaches.end()) {
                originIt->value.cacheIdentifiers.removeFirst(cacheIdentifier);
                ++originIt->value.updateCounter;
            }
            m_caches.remove(it);
            return CacheIdentifierOperationResult { cacheIdentifier, false };
        }();
        callback(result); // lock released before callback (may re-enter the store)
    }

    void retrieveCaches(const ClientOrigin& origin, CacheInfosCallback&& callback)
    {
        auto result = [&]() -> CacheInfos {
            Locker locker { m_lock };
            auto originIt = m_originCaches.find(origin);
            if (originIt == m_originCaches.end())
                return CacheInfos { { }, 0 };
            Vector<CacheInfo> infos;
            infos.reserveInitialCapacity(originIt->value.cacheIdentifiers.size());
            for (auto id : originIt->value.cacheIdentifiers) {
                auto it = m_caches.find(id);
                if (it != m_caches.end())
                    infos.append(CacheInfo { id, it->value->name.isolatedCopy() });
            }
            return CacheInfos { WTFMove(infos), originIt->value.updateCounter };
        }();
        // Lock released: this callback synchronously calls back into open() (the deadlock we fixed).
        callback(WTFMove(result));
    }

    void retrieveRecords(uint64_t cacheIdentifier, const RetrieveRecordsOptions& options, RecordsCallback&& callback)
    {
        auto results = [&]() -> Vector<Record> {
            Locker locker { m_lock };
            auto it = m_caches.find(cacheIdentifier);
            if (it == m_caches.end())
                return Vector<Record> { };
            auto& entry = *it->value;

            Vector<Record> out;
            auto appendRecord = [&](const Record& record) {
                Record copy = record.copy();
                if (!options.shouldProvideResponse) {
                    copy.response = { };
                    copy.responseBody = nullptr;
                    copy.responseBodySize = 0;
                }
                out.append(WTFMove(copy));
            };

            // Null request URL => match-all (matchAll()/keys() with no argument).
            if (options.request.url().isNull()) {
                for (auto& record : entry.records)
                    appendRecord(record);
                return out;
            }

            if (!options.ignoreMethod && options.request.httpMethod() != "GET")
                return out;

            CacheQueryOptions queryOptions { options.ignoreSearch, options.ignoreMethod, options.ignoreVary, { } };
            for (auto& record : entry.records) {
                if (queryCacheMatch(options.request, record.request, record.response, queryOptions))
                    appendRecord(record);
            }
            return out;
        }();
        callback(WTFMove(results)); // lock released before callback
    }

    void batchPutOperation(uint64_t cacheIdentifier, Vector<Record>&& records, RecordIdentifiersCallback&& callback)
    {
        auto result = [&]() -> RecordIdentifiersOrError {
            Locker locker { m_lock };
            auto it = m_caches.find(cacheIdentifier);
            if (it == m_caches.end())
                return makeUnexpected(Error::Internal);
            auto& entry = *it->value;

            Vector<uint64_t> identifiers;
            CacheQueryOptions queryOptions { }; // exact match (URL incl. query + method + vary)
            for (auto& record : records) {
                size_t position = entry.records.findIf([&](const Record& existing) {
                    return queryCacheMatch(record.request, existing.request, existing.response, queryOptions);
                });
                if (position != notFound) {
                    // Overwrite in place, preserving the existing record identifier.
                    uint64_t existingIdentifier = entry.records[position].identifier;
                    record.identifier = existingIdentifier;
                    record.updateResponseCounter = entry.records[position].updateResponseCounter + 1;
                    entry.records[position] = WTFMove(record);
                    identifiers.append(existingIdentifier);
                } else {
                    uint64_t recordIdentifier = ++entry.nextRecordIdentifier;
                    record.identifier = recordIdentifier;
                    identifiers.append(recordIdentifier);
                    entry.records.append(WTFMove(record));
                }
            }
            return WTFMove(identifiers);
        }();
        callback(WTFMove(result)); // lock released before callback
    }

    void batchDeleteOperation(uint64_t cacheIdentifier, const ResourceRequest& request, CacheQueryOptions&& options, RecordIdentifiersCallback&& callback)
    {
        auto deletedIdentifiers = [&]() -> Vector<uint64_t> {
            Locker locker { m_lock };
            auto it = m_caches.find(cacheIdentifier);
            if (it == m_caches.end())
                return Vector<uint64_t> { };
            auto& entry = *it->value;

            if (!options.ignoreMethod && request.httpMethod() != "GET")
                return Vector<uint64_t> { };

            Vector<uint64_t> deleted;
            entry.records.removeAllMatching([&](const Record& record) {
                if (queryCacheMatch(request, record.request, record.response, options)) {
                    deleted.append(record.identifier);
                    return true;
                }
                return false;
            });
            return deleted;
        }();
        callback(WTFMove(deletedIdentifiers)); // lock released before callback
    }

private:
    PortCacheStore() = default;

    Lock m_lock;
    HashMap<uint64_t, std::unique_ptr<PortCacheEntry>> m_caches; // by cache identifier
    HashMap<ClientOrigin, PortOriginCaches> m_originCaches;
    uint64_t m_nextCacheIdentifier { 0 };
};

// The connection WebCore talks to. Every method forwards to the shared store synchronously
// (in-process — no IPC round trip) and invokes its completion handler exactly once.
class PortCacheStorageConnection final : public CacheStorageConnection {
public:
    static Ref<PortCacheStorageConnection> create(Ref<PortCacheStore>&& store)
    {
        return adoptRef(*new PortCacheStorageConnection(WTFMove(store)));
    }

private:
    explicit PortCacheStorageConnection(Ref<PortCacheStore>&& store)
        : m_store(WTFMove(store))
    {
    }

    void open(const ClientOrigin& origin, const String& cacheName, CacheIdentifierCallback&& callback) final
    {
        m_store->open(origin, cacheName, WTFMove(callback));
    }

    void remove(uint64_t cacheIdentifier, CacheIdentifierCallback&& callback) final
    {
        m_store->remove(cacheIdentifier, WTFMove(callback));
    }

    void retrieveCaches(const ClientOrigin& origin, uint64_t /* updateCounter */, CacheInfosCallback&& callback) final
    {
        // We always report our current updateCounter; DOMCacheStorage refreshes only when
        // it differs from what it last saw, so passing the client's counter through is
        // unnecessary.
        m_store->retrieveCaches(origin, WTFMove(callback));
    }

    void retrieveRecords(uint64_t cacheIdentifier, const RetrieveRecordsOptions& options, RecordsCallback&& callback) final
    {
        m_store->retrieveRecords(cacheIdentifier, options, WTFMove(callback));
    }

    void batchDeleteOperation(uint64_t cacheIdentifier, const ResourceRequest& request, CacheQueryOptions&& options, RecordIdentifiersCallback&& callback) final
    {
        m_store->batchDeleteOperation(cacheIdentifier, request, WTFMove(options), WTFMove(callback));
    }

    void batchPutOperation(uint64_t cacheIdentifier, Vector<Record>&& records, RecordIdentifiersCallback&& callback) final
    {
        m_store->batchPutOperation(cacheIdentifier, WTFMove(records), WTFMove(callback));
    }

    // In-memory: caches live until explicitly removed, so ref/deref of a cache handle are
    // no-ops (there is no engine-side lifetime to pin/release).
    void reference(uint64_t /* cacheIdentifier */) final { }
    void dereference(uint64_t /* cacheIdentifier */) final { }

    Ref<PortCacheStore> m_store;
};

// The provider. Holds one shared store + connection so every createCacheStorageConnection()
// (once per page) sees the same caches for the provider's lifetime.
class PortCacheStorageProvider final : public CacheStorageProvider {
public:
    static Ref<PortCacheStorageProvider> create() { return adoptRef(*new PortCacheStorageProvider); }

    Ref<CacheStorageConnection> createCacheStorageConnection() final
    {
        return m_connection.copyRef();
    }

private:
    PortCacheStorageProvider()
        : m_store(PortCacheStore::create())
        , m_connection(PortCacheStorageConnection::create(m_store.copyRef()))
    {
    }

    Ref<PortCacheStore> m_store;
    Ref<PortCacheStorageConnection> m_connection;
};

Ref<CacheStorageProvider> createPortCacheStorageProvider()
{
    return PortCacheStorageProvider::create();
}

} // namespace WebCorePort
