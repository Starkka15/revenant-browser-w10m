// PortEditorClient — a real EditorClient for the Revenant port. The EmptyEditorClient that
// pageConfigurationWithEmptyClients installs is a no-op (handleKeyboardEvent does nothing and
// shouldBeginEditing/shouldInsertText return false), so typing into web text fields inserts
// nothing. This client permits editing and turns key events into text insertion / editing
// commands so <input>/<textarea>/contenteditable actually work.
#pragma once

#include <wtf/UniqueRef.h>

namespace WebCore { class EditorClient; }

namespace WebCorePort {
WTF::UniqueRef<WebCore::EditorClient> createPortEditorClient();
}
