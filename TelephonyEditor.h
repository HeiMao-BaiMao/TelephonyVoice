#pragma once

#include "vstgui/plugin-bindings/vst3editor.h"
#include <memory>
#include <string>
#include <unordered_map>

namespace Steinberg::Vst {

// Uses the SDK editor's parameter listeners for host automation, text entry,
// context menus, keyboard navigation and HiDPI. This delegate only manages
// presentation; it never reads the audio thread or changes a parameter.
class TelephonyEditor final : public VSTGUI::VST3Editor,
                              private VSTGUI::VST3EditorDelegate {
public:
    explicit TelephonyEditor(EditController* controller);
    ~TelephonyEditor() override;

private:
    class Observer;
    std::unique_ptr<Observer> observer;
    std::unordered_map<std::string, VSTGUI::CView*> namedViews;
    std::unordered_map<std::string, VSTGUI::CView*> groups;
    bool refreshing = false;

    VSTGUI::CView* createCustomView(VSTGUI::UTF8StringPtr name,
                                   const VSTGUI::UIAttributes& attributes,
                                   const VSTGUI::IUIDescription* description,
                                   VSTGUI::VST3Editor* editor) override;
    VSTGUI::CView* verifyView(VSTGUI::CView* view,
                             const VSTGUI::UIAttributes& attributes,
                             const VSTGUI::IUIDescription* description,
                             VSTGUI::VST3Editor* editor) override;
    void didOpen(VSTGUI::VST3Editor* editor) override;
    void willClose(VSTGUI::VST3Editor* editor) override;
    void refresh();
    void setText(const char* name, const char* text);
    void setGroupEnabled(const char* name, bool enabled);
    void setGroupVisible(const char* name, bool visible);
};

} // namespace Steinberg::Vst
