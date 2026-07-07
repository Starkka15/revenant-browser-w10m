// PortEditorClient — see PortEditorClient.h. Structure mirrors WebCore's EmptyEditorClient
// (loader/EmptyClients.cpp) so every EditorClient pure-virtual is satisfied on this platform;
// the ONLY behavioural changes are: editing is permitted (shouldBeginEditing / shouldInsertText /
// shouldDeleteRange / shouldChangeSelectedRange return true) and handleKeyboardEvent actually
// interprets keys into text insertion + editing commands.
#include "config.h"
#include "PortEditorClient.h"

#include "DOMPasteAccess.h"
#include "Document.h"
#include "DocumentFragment.h"
#include "Editor.h"
#include "EditorClient.h"
#include "Element.h"
#include "Event.h"
#include "Frame.h"
#include "KeyboardEvent.h"
#include "Node.h"
#include "PlatformKeyboardEvent.h"
#include "SharedBuffer.h"
#include "SimpleRange.h"
#include "StyleProperties.h"
#include "TextCheckerClient.h"
#include "UndoStep.h"
#include "VisibleSelection.h"

namespace WebCore {

class PortEditorClient final : public EditorClient {
    WTF_MAKE_FAST_ALLOCATED;
public:
    PortEditorClient() = default;

private:
    // --- Editing permissions: allow it (the whole point vs EmptyEditorClient). ---
    bool shouldDeleteRange(const std::optional<SimpleRange>&) final { return true; }
    bool smartInsertDeleteEnabled() final { return false; }
    bool isSelectTrailingWhitespaceEnabled() const final { return false; }
    bool isContinuousSpellCheckingEnabled() final { return false; }
    void toggleContinuousSpellChecking() final { }
    bool isGrammarCheckingEnabled() final { return false; }
    void toggleGrammarChecking() final { }
    int spellCheckerDocumentTag() final { return -1; }

    bool shouldBeginEditing(const SimpleRange&) final { return true; }
    bool shouldEndEditing(const SimpleRange&) final { return true; }
    bool shouldInsertNode(Node&, const std::optional<SimpleRange>&, EditorInsertAction) final { return true; }
    bool shouldInsertText(const String&, const std::optional<SimpleRange>&, EditorInsertAction) final { return true; }
    bool shouldChangeSelectedRange(const std::optional<SimpleRange>&, const std::optional<SimpleRange>&, Affinity, bool) final { return true; }

    bool shouldApplyStyle(const StyleProperties&, const std::optional<SimpleRange>&) final { return true; }
    void didApplyStyle() final { }
    bool shouldMoveRangeAfterDelete(const SimpleRange&, const SimpleRange&) final { return true; }

    void didBeginEditing() final { }
    void respondToChangedContents() final { }
    void respondToChangedSelection(Frame*) final { }
    void updateEditorStateAfterLayoutIfEditabilityChanged() final { }
    void discardedComposition(Frame*) final { }
    void canceledComposition() final { }
    void didUpdateComposition() final { }
    void didEndEditing() final { }
    void didEndUserTriggeredSelectionChanges() final { }
    void willWriteSelectionToPasteboard(const std::optional<SimpleRange>&) final { }
    void didWriteSelectionToPasteboard() final { }
    void getClientPasteboardData(const std::optional<SimpleRange>&, Vector<String>&, Vector<RefPtr<SharedBuffer>>&) final { }
    void requestCandidatesForSelection(const VisibleSelection&) final { }
    void handleAcceptedCandidateWithSoftSpaces(TextCheckingResult) final { }

    void registerUndoStep(UndoStep&) final { }
    void registerRedoStep(UndoStep&) final { }
    void clearUndoRedoOperations() final { }

    DOMPasteAccessResponse requestDOMPasteAccess(DOMPasteAccessCategory, const String&) final { return DOMPasteAccessResponse::DeniedForGesture; }

    bool canCopyCut(Frame*, bool defaultValue) const final { return defaultValue; }
    bool canPaste(Frame*, bool defaultValue) const final { return defaultValue; }
    bool canUndo() const final { return false; }
    bool canRedo() const final { return false; }

    void undo() final { }
    void redo() final { }

    // --- The real work: turn key events into edits. ---
    void handleKeyboardEvent(KeyboardEvent&) final;
    void handleInputMethodKeydown(KeyboardEvent&) final { }

    void textFieldDidBeginEditing(Element*) final { }
    void textFieldDidEndEditing(Element*) final { }
    void textDidChangeInTextField(Element*) final { }
    bool doTextFieldCommandFromEvent(Element*, KeyboardEvent*) final { return false; }
    void textWillBeDeletedInTextField(Element*) final { }
    void textDidChangeInTextArea(Element*) final { }
    void overflowScrollPositionChanged() final { }
    void subFrameScrollPositionChanged() final { }

#if PLATFORM(IOS_FAMILY)
    void startDelayingAndCoalescingContentChangeNotifications() final { }
    void stopDelayingAndCoalescingContentChangeNotifications() final { }
    bool hasRichlyEditableSelection() final { return false; }
    int getPasteboardItemsCount() final { return 0; }
    RefPtr<DocumentFragment> documentFragmentFromDelegate(int) final { return nullptr; }
    bool performsTwoStepPaste(DocumentFragment*) final { return false; }
    void updateStringForFind(const String&) final { }
#endif

    bool performTwoStepDrop(DocumentFragment&, const SimpleRange&, bool) final { return false; }

#if PLATFORM(COCOA)
    void setInsertionPasteboard(const String&) final { };
#endif

#if USE(APPKIT)
    void uppercaseWord() final { }
    void lowercaseWord() final { }
    void capitalizeWord() final { }
#endif

#if USE(AUTOMATIC_TEXT_REPLACEMENT)
    void showSubstitutionsPanel(bool) final { }
    bool substitutionsPanelIsShowing() final { return false; }
    void toggleSmartInsertDelete() final { }
    bool isAutomaticQuoteSubstitutionEnabled() final { return false; }
    void toggleAutomaticQuoteSubstitution() final { }
    bool isAutomaticLinkDetectionEnabled() final { return false; }
    void toggleAutomaticLinkDetection() final { }
    bool isAutomaticDashSubstitutionEnabled() final { return false; }
    void toggleAutomaticDashSubstitution() final { }
    bool isAutomaticTextReplacementEnabled() final { return false; }
    void toggleAutomaticTextReplacement() final { }
    bool isAutomaticSpellingCorrectionEnabled() final { return false; }
    void toggleAutomaticSpellingCorrection() final { }
#endif

#if PLATFORM(GTK)
    bool shouldShowUnicodeMenu() final { return false; }
#endif

    TextCheckerClient* textChecker() final { return &m_textCheckerClient; }

    void updateSpellingUIWithGrammarString(const String&, const GrammarDetail&) final { }
    void updateSpellingUIWithMisspelledWord(const String&) final { }
    void showSpellingUI(bool) final { }
    bool spellingUIIsShowing() final { return false; }

    void willSetInputMethodState() final { }
    void setInputMethodState(Element*) final { }

    bool canShowFontPanel() const final { return false; }

    class PortTextCheckerClient final : public TextCheckerClient {
        bool shouldEraseMarkersAfterChangeSelection(TextCheckingType) const final { return true; }
        void ignoreWordInSpellDocument(const String&) final { }
        void learnWord(const String&) final { }
        void checkSpellingOfString(StringView, int*, int*) final { }
        String getAutoCorrectSuggestionForMisspelledWord(const String&) final { return { }; }
        void checkGrammarOfString(StringView, Vector<GrammarDetail>&, int*, int*) final { }
#if USE(UNIFIED_TEXT_CHECKING)
        Vector<TextCheckingResult> checkTextOfParagraph(StringView, OptionSet<TextCheckingType>, const VisibleSelection&) final { return Vector<TextCheckingResult>(); }
#endif
        void getGuessesForWord(const String&, const String&, const VisibleSelection&, Vector<String>&) final { }
        void requestCheckingOfString(TextCheckingRequest&, const VisibleSelection&) final { }
    };

    PortTextCheckerClient m_textCheckerClient;
};

void PortEditorClient::handleKeyboardEvent(KeyboardEvent& event)
{
    auto* target = event.target();
    if (!is<Node>(target))
        return;
    Frame* frame = downcast<Node>(*target).document().frame();
    if (!frame)
        return;

    const PlatformKeyboardEvent* platformEvent = event.underlyingPlatformEvent();
    if (!platformEvent)
        return;

    // keypress (Char): insert the typed character. Skip control chars and modifier chords
    // (Ctrl/Alt combos are commands, not text); plain Shift is fine (capital letters).
    if (platformEvent->type() == PlatformEvent::Char) {
        if (platformEvent->controlKey() || platformEvent->altKey())
            return;
        String text = platformEvent->text();
        if (text.isEmpty())
            return;
        if (text.length() == 1 && text[0] < ' ')
            return;
        if (frame->editor().insertText(text, &event))
            event.setDefaultHandled();
        return;
    }

    // keydown (RawKeyDown): editing/navigation keys. Enter and Tab are deliberately NOT handled
    // here so the DOM default (form submit / focus move / textarea newline) runs instead.
    if (platformEvent->type() == PlatformEvent::RawKeyDown) {
        bool shift = platformEvent->shiftKey();
        String command;
        switch (platformEvent->windowsVirtualKeyCode()) {
        case 0x08: command = "DeleteBackward"_s; break;                                            // Backspace
        case 0x2E: command = "DeleteForward"_s; break;                                             // Delete
        case 0x25: command = shift ? "MoveLeftAndModifySelection"_s : "MoveLeft"_s; break;         // Left
        case 0x27: command = shift ? "MoveRightAndModifySelection"_s : "MoveRight"_s; break;       // Right
        case 0x26: command = shift ? "MoveUpAndModifySelection"_s : "MoveUp"_s; break;             // Up
        case 0x28: command = shift ? "MoveDownAndModifySelection"_s : "MoveDown"_s; break;         // Down
        case 0x24: command = shift ? "MoveToBeginningOfLineAndModifySelection"_s : "MoveToBeginningOfLine"_s; break; // Home
        case 0x23: command = shift ? "MoveToEndOfLineAndModifySelection"_s : "MoveToEndOfLine"_s; break;             // End
        default: break;
        }
        if (!command.isEmpty() && frame->editor().command(command).execute())
            event.setDefaultHandled();
        return;
    }
}

} // namespace WebCore

namespace WebCorePort {
WTF::UniqueRef<WebCore::EditorClient> createPortEditorClient()
{
    return WTF::makeUniqueRef<WebCore::PortEditorClient>();
}
}
