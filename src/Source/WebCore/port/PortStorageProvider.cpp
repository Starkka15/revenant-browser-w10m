#include "config.h"
#include "PortStorageProvider.h"

#include "Page.h"
#include "SQLiteDatabase.h"
#include "SQLiteStatement.h"
#include "SecurityOrigin.h"
#include "StorageArea.h"
#include "StorageNamespace.h"
#include "StorageNamespaceProvider.h"
#include "StorageSessionProvider.h"
#include "StorageType.h"
#include <string>
#include <wtf/HashMap.h>
#include <wtf/NeverDestroyed.h>
#include <cstdio> // snprintf for localStorage persistence diagnostics
#include <wtf/Vector.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

namespace WebCore { NetworkStorageSession& portSharedStorageSession(); }

namespace WebCorePort {
using namespace WebCore;

const std::string& portDataPath();
void portLog(const char*);

// Disk backing for localStorage. One SQLite file (<LocalFolder>\localstorage.db) with a single
// (origin, key, value) table; every origin's StorageArea write-throughs here and loads from here
// on first access, so localStorage survives app restart (a real browser behavior). sessionStorage
// and transient storage stay in-memory (correct — they are per-session by spec). If no data path
// was supplied by the shell, this stays closed and everything falls back to memory-only.
class LocalStorageDB {
public:
    static LocalStorageDB& singleton() { static NeverDestroyed<LocalStorageDB> db; return db.get(); }

    bool ensureOpen()
    {
        if (m_triedOpen)
            return m_db.isOpen();
        m_triedOpen = true;
        const std::string& dp = portDataPath();
        if (dp.empty())
            return false;
        std::string full = dp + "\\localstorage.db";
        String path = String::fromUTF8(full.c_str());
        bool ok = m_db.open(path);
        if (!ok) {
            char b[300];
            snprintf(b, sizeof b, "localstorage: OPEN FAILED '%s' err=%d msg=%s", full.c_str(), m_db.lastError(), m_db.lastErrorMsg());
            portLog(b);
            return false;
        }
        // Force rollback-journal (DELETE) instead of WAL. WAL needs -wal/-shm shared-memory files
        // that are unreliable inside the W10M AppContainer, so WAL writes can fail to checkpoint
        // into the main .db and are lost on next launch — exactly the "my age-confirm doesn't
        // stick" symptom. DELETE mode writes straight to the main db file, which persists.
        m_db.executeCommand("PRAGMA journal_mode=DELETE"_s);
        m_db.executeCommand("CREATE TABLE IF NOT EXISTS ItemTable (origin TEXT NOT NULL, key TEXT NOT NULL, value TEXT NOT NULL, PRIMARY KEY(origin, key))"_s);
        { char b[220]; snprintf(b, sizeof b, "localstorage: opened '%s' ok", full.c_str()); portLog(b); }
        return m_db.isOpen();
    }

    void load(const String& origin, HashMap<String, String>& map, Vector<String>& order)
    {
        if (!ensureOpen())
            return;
        auto stmt = m_db.prepareStatement("SELECT key, value FROM ItemTable WHERE origin = ?"_s);
        if (!stmt)
            return;
        stmt->bindText(1, origin);
        int n = 0;
        while (stmt->step() == SQLITE_ROW) {
            String key = stmt->columnText(0);
            if (map.set(key, stmt->columnText(1)).isNewEntry)
                order.append(key);
            ++n;
        }
        char b[220]; snprintf(b, sizeof b, "localstorage: load origin=%s -> %d items", origin.utf8().data(), n); portLog(b);
    }

    void setItem(const String& origin, const String& key, const String& value)
    {
        if (!ensureOpen())
            return;
        auto stmt = m_db.prepareStatement("INSERT OR REPLACE INTO ItemTable (origin, key, value) VALUES (?, ?, ?)"_s);
        if (!stmt)
            return;
        stmt->bindText(1, origin);
        stmt->bindText(2, key);
        stmt->bindText(3, value);
        int rc = stmt->step();
        char b[220]; snprintf(b, sizeof b, "localstorage: setItem origin=%s key=%s rc=%d", origin.utf8().data(), key.utf8().data(), rc); portLog(b);
    }

    void removeItem(const String& origin, const String& key)
    {
        if (!ensureOpen())
            return;
        auto stmt = m_db.prepareStatement("DELETE FROM ItemTable WHERE origin = ? AND key = ?"_s);
        if (!stmt)
            return;
        stmt->bindText(1, origin);
        stmt->bindText(2, key);
        stmt->step();
    }

    void clear(const String& origin)
    {
        if (!ensureOpen())
            return;
        auto stmt = m_db.prepareStatement("DELETE FROM ItemTable WHERE origin = ?"_s);
        if (!stmt)
            return;
        stmt->bindText(1, origin);
        stmt->step();
    }

private:
    SQLiteDatabase m_db;
    bool m_triedOpen { false };
};

// Real StorageArea. The EmptyClients StorageArea silently drops every write and returns null for
// every read, which makes localStorage-backed SPAs loop/stall. Local storage is PERSISTED to
// LocalStorageDB (write-through + load-on-open) so it survives restart; session/transient stay
// in-memory (per-session by spec). m_map is the authoritative in-memory copy for fast reads.
class PortStorageArea final : public StorageArea {
public:
    static Ref<PortStorageArea> create(StorageType type, const String& origin) { return adoptRef(*new PortStorageArea(type, origin)); }

    unsigned length() final { return m_order.size(); }
    String key(unsigned index) final { return index < m_order.size() ? m_order[index] : String(); }
    String item(const String& key) final { return m_map.get(key); }

    void setItem(Frame*, const String& key, const String& value, bool& quotaException) final
    {
        quotaException = false;
        if (m_map.set(key, value).isNewEntry)
            m_order.append(key);
        if (m_persistent)
            LocalStorageDB::singleton().setItem(m_origin, key, value);
    }

    void removeItem(Frame*, const String& key) final
    {
        if (m_map.remove(key))
            m_order.removeFirst(key);
        if (m_persistent)
            LocalStorageDB::singleton().removeItem(m_origin, key);
    }

    void clear(Frame*) final
    {
        m_map.clear();
        m_order.clear();
        if (m_persistent)
            LocalStorageDB::singleton().clear(m_origin);
    }

    bool contains(const String& key) final { return m_map.contains(key); }
    StorageType storageType() const final { return m_type; }

    size_t memoryBytesUsedByCache() final
    {
        size_t bytes = 0;
        for (auto& entry : m_map)
            bytes += (entry.key.length() + entry.value.length()) * sizeof(UChar);
        return bytes;
    }

private:
    PortStorageArea(StorageType type, const String& origin)
        : m_origin(origin)
        , m_type(type)
    {
        // Only real localStorage persists. Ephemeral fallback (no data path) keeps m_persistent
        // false via ensureOpen() failing, so this degrades cleanly to memory-only.
        if (m_type == StorageType::Local && LocalStorageDB::singleton().ensureOpen()) {
            m_persistent = true;
            LocalStorageDB::singleton().load(m_origin, m_map, m_order);
        }
    }
    String m_origin;
    HashMap<String, String> m_map;
    Vector<String> m_order;
    StorageType m_type;
    bool m_persistent { false };
};

class PortStorageNamespace final : public StorageNamespace {
public:
    static Ref<PortStorageNamespace> create(StorageType type, PAL::SessionID sessionID) { return adoptRef(*new PortStorageNamespace(type, sessionID)); }

    Ref<StorageArea> storageArea(const SecurityOrigin& origin) final
    {
        auto originStr = origin.toString();
        auto& area = m_areas.ensure(originStr, [&] { return PortStorageArea::create(m_type, originStr); }).iterator->value;
        return *area;
    }

    Ref<StorageNamespace> copy(Page&) final
    {
        // sessionStorage duplication for a cloned page; start empty (adequate: our shell is
        // single-page and never clones). Local storage is shared via the provider cache.
        return create(m_type, m_sessionID);
    }

    PAL::SessionID sessionID() const final { return m_sessionID; }
    void setSessionIDForTesting(PAL::SessionID id) final { m_sessionID = id; }

private:
    PortStorageNamespace(StorageType type, PAL::SessionID sessionID) : m_type(type), m_sessionID(sessionID) { }
    HashMap<String, RefPtr<PortStorageArea>> m_areas;
    StorageType m_type;
    PAL::SessionID m_sessionID;
};

class PortStorageNamespaceProvider final : public StorageNamespaceProvider {
public:
    static Ref<PortStorageNamespaceProvider> create() { return adoptRef(*new PortStorageNamespaceProvider); }

private:
    Ref<StorageNamespace> createSessionStorageNamespace(Page& page, unsigned) final
    {
        return PortStorageNamespace::create(StorageType::Session, page.sessionID());
    }
    Ref<StorageNamespace> createLocalStorageNamespace(unsigned, PAL::SessionID sessionID) final
    {
        return PortStorageNamespace::create(StorageType::Local, sessionID);
    }
    Ref<StorageNamespace> createTransientLocalStorageNamespace(SecurityOrigin&, unsigned, PAL::SessionID sessionID) final
    {
        return PortStorageNamespace::create(StorageType::TransientLocal, sessionID);
    }
};

// Cookie StorageSessionProvider — returns the SAME network session the HTTP loader uses so
// document.cookie and Set-Cookie share one jar. EmptyStorageSessionProvider returns nullptr.
class PortStorageSessionProvider final : public StorageSessionProvider {
public:
    static Ref<PortStorageSessionProvider> create() { return adoptRef(*new PortStorageSessionProvider); }
private:
    NetworkStorageSession* storageSession() const final { return &portSharedStorageSession(); }
};

Ref<StorageNamespaceProvider> createPortStorageNamespaceProvider() { return PortStorageNamespaceProvider::create(); }
Ref<StorageSessionProvider> createPortStorageSessionProvider() { return PortStorageSessionProvider::create(); }

} // namespace WebCorePort
