#include "TelephonyEditor.h"
#include "TelephonyVoice.h"
#include "dsp/EvsConfig.h"
#include "base/source/fobject.h"
#include "pluginterfaces/gui/iplugview.h"
#include "vstgui/lib/cframe.h"
#include "vstgui/lib/cdrawcontext.h"
#include "vstgui/lib/cviewcontainer.h"
#include "vstgui/lib/controls/cbuttons.h"
#include "vstgui/lib/controls/coptionmenu.h"
#include "vstgui/lib/controls/cslider.h"
#include "vstgui/lib/controls/ctextedit.h"
#include "vstgui/uidescription/uiattributes.h"
#include <cmath>
#include <cstring>
#include <vector>

namespace Steinberg::Vst {
namespace {

// Vector-only thumb/track rendering. All editing, keyboard, wheel, fine-adjust
// and automation gesture behavior stays in the SDK's standard slider class.
class RouteSlider final : public VSTGUI::CSlider {
public:
    RouteSlider() : CSlider(VSTGUI::CRect(0, 0, 100, 20), nullptr, -1,
                           0, 100, nullptr, nullptr)
    {
        setHandleSizePrivate(14, 14);
    }

    void draw(VSTGUI::CDrawContext* context) override
    {
        using namespace VSTGUI;
        const auto bounds = getViewSize();
        const auto y = (bounds.top + bounds.bottom) * 0.5;
        const auto left = bounds.left + 7;
        const auto right = bounds.right - 7;
        const auto x = left + (right - left) * getValueNormalized();
        context->setDrawMode(kAntiAliasing);
        context->setFillColor(getBackColor());
        context->drawRect(CRect(left, y - 2, right, y + 2), kDrawFilled);
        context->setFillColor(getValueColor());
        context->drawRect(CRect(left, y - 2, x, y + 2), kDrawFilled);
        context->setFrameColor(getFrameColor());
        context->setLineWidth(1);
        context->drawEllipse(CRect(x - 7, y - 7, x + 7, y + 7), kDrawFilledAndStroked);
        setDirty(false);
    }
};

// Keep route readouts based on the controller's actual list, not a duplicate
// normalized denominator. Host sessions and build variants share parameter IDs.
int choice(EditController* controller, ParamID id)
{
    return static_cast<int>(std::lround(controller->normalizedParamToPlain(
        id, controller->getParamNormalized(id))));
}

#ifndef TELEPHONY_DISTRIBUTION_BUILD
bool evsBitrateIsCapped(EditController* controller)
{
    // StringListParameter plain values are indices. These tables mirror the
    // processor's public choice lists; ValidateEditor.py checks them for drift.
    static constexpr int sampleRates[] = {8000, 16000, 32000, 48000};
    static constexpr int bitrates[] = {
        5900, 7200, 8000, 9600, 13200, 16400, 24400, 32000, 48000, 64000, 96000, 128000
    };
    const auto rateIndex = choice(controller, kParamEvsSampleRate);
    const auto bitrateIndex = choice(controller, kParamEvsBitrate);
    if (rateIndex < 0 || rateIndex >= 4 || bitrateIndex < 0 || bitrateIndex >= 12)
        return true;
    auto sampleRate = sampleRates[rateIndex];
    auto bitrate = bitrates[bitrateIndex];
    auto bandwidth = static_cast<EVS_Bandwidth>(choice(controller, kParamEvsMaxBw));
    // Use the very same deterministic frontend normalization as the DSP.
    // Keep the selected request visible, but clearly disclose when the actual
    // nominal bitrate is capped. The separate Force VBR override comes later.
    TelephonyDSP::normalizeEvsConfig(sampleRate, bitrate, bandwidth);
    return bitrate != bitrates[bitrateIndex];
}
#endif

const char* codecForEndpoint(int endpoint)
{
    switch (endpoint) {
        case 0: return "G.711 / 8 kHz";
        case 1: return "GSM FR / 8 kHz";
#ifdef TELEPHONY_DISTRIBUTION_BUILD
        case 2: return "5G approximation / filter only";
#else
        case 2: return "AMR-NB / 8 kHz";
        case 3: return "AMR-WB / 16 kHz";
        case 4: return "5G approximation / filter only";
        case 5: return "EVS / configurable bandwidth";
#if TELEPHONY_USE_EVS_JBM
        case 6: return "EVS / simulated jitter buffer";
#endif
#endif
        default: return "Unknown endpoint";
    }
}

bool isInteractive(VSTGUI::CView* view)
{
    return dynamic_cast<VSTGUI::COptionMenu*>(view) ||
           dynamic_cast<VSTGUI::CSlider*>(view) ||
           dynamic_cast<VSTGUI::CTextEdit*>(view) ||
           dynamic_cast<VSTGUI::CTextButton*>(view);
}

void enableChildren(VSTGUI::CView* view, bool enabled, VSTGUI::CFrame* frame)
{
    if (isInteractive(view)) {
        // A disabled group also blocks mouse dispatch at its container. The
        // native parameter listener may independently restore a child's mouse
        // flag during automation, so explicitly suppress keyboard focus too.
        if (!enabled && frame && frame->getFocusView() == view)
            frame->setFocusView(nullptr);
        if (!enabled) {
            if (auto* control = dynamic_cast<VSTGUI::CControl*>(view))
                while (control->isEditing())
                    control->endEdit();
        }
        view->setWantsFocus(enabled);
        view->setMouseEnabled(enabled);
    }
    if (auto* container = view->asViewContainer()) {
        container->forEachChild([&](VSTGUI::CView* child) {
            enableChildren(child, enabled, frame);
        });
    }
}

} // namespace

class TelephonyEditor::Observer final : public FObject {
public:
    explicit Observer(TelephonyEditor& owner) : owner(owner)
    {
        auto* controller = owner.getController();
        for (int32 index = 0; index < controller->getParameterCount(); ++index) {
            ParameterInfo info {};
            if (controller->getParameterInfo(index, info) != kResultOk)
                continue;
            if (auto* parameter = controller->getParameterObject(info.id)) {
                parameter->addRef();
                parameter->addDependent(this);
                parameters.push_back(parameter);
            }
        }
    }

    ~Observer() override
    {
        for (auto* parameter : parameters) {
            parameter->removeDependent(this);
            parameter->release();
        }
    }

    void PLUGIN_API update(FUnknown*, int32 message) override
    {
        if (message == IDependent::kChanged)
            owner.refresh();
    }

private:
    TelephonyEditor& owner;
    std::vector<Parameter*> parameters;
};

TelephonyEditor::TelephonyEditor(EditController* controller)
    : VST3Editor(controller, "view", "telephonyvoice.uidesc")
{
    setDelegate(this);
    setAllowedZoomFactors({0.75, 1.0, 1.25, 1.5, 2.0});
    enableTooltips(true);
    // Do not expose the SDK's live layout editor in a production plug-in.
    enableShowEditButton(false);
}

TelephonyEditor::~TelephonyEditor()
{
    // Some hosts destroy an attached editor without first calling removed().
    // Tear down our dependents before the SDK destroys the frame/controller.
    if (observer)
        VST3Editor::close();
    observer.reset();
    setDelegate(nullptr);
}

VSTGUI::CView* TelephonyEditor::createCustomView(
    VSTGUI::UTF8StringPtr name, const VSTGUI::UIAttributes&,
    const VSTGUI::IUIDescription*, VSTGUI::VST3Editor*)
{
    return name && std::strcmp(name, "TelephonySlider") == 0 ? new RouteSlider() : nullptr;
}

VSTGUI::CView* TelephonyEditor::verifyView(
    VSTGUI::CView* view, const VSTGUI::UIAttributes& attributes,
    const VSTGUI::IUIDescription*, VSTGUI::VST3Editor*)
{
    if (const auto* name = attributes.getAttributeValue("telephony-name"))
        namedViews[*name] = view;
    if (const auto* group = attributes.getAttributeValue("telephony-group"))
        groups[*group] = view;
    return view;
}

void TelephonyEditor::didOpen(VSTGUI::VST3Editor*)
{
    observer = std::make_unique<Observer>(*this);
    refresh();
}

void TelephonyEditor::willClose(VSTGUI::VST3Editor*)
{
    observer.reset();
    if (auto* frame = getFrame()) {
        // Finish text, drag and wheel gestures while the native parameter
        // listeners still exist. Closing during an edit must not leave the
        // host with an unmatched beginEdit notification.
        frame->setFocusView(nullptr);
        const auto finishEdits = [&](auto&& self, VSTGUI::CView* view) -> void {
            if (auto* control = dynamic_cast<VSTGUI::CControl*>(view))
                while (control->isEditing())
                    control->endEdit();
            if (auto* container = view->asViewContainer())
                container->forEachChild([&](VSTGUI::CView* child) { self(self, child); });
        };
        finishEdits(finishEdits, frame);
    }
    namedViews.clear();
    groups.clear();
}

void TelephonyEditor::setText(const char* name, const char* text)
{
    const auto found = namedViews.find(name);
    if (found == namedViews.end())
        return;
    if (auto* label = dynamic_cast<VSTGUI::CTextLabel*>(found->second))
        label->setText(text);
    else if (auto* button = dynamic_cast<VSTGUI::CTextButton*>(found->second))
        button->setTitle(text);
}

void TelephonyEditor::setGroupEnabled(const char* name, bool enabled)
{
    const auto found = groups.find(name);
    if (found == groups.end())
        return;
    found->second->setMouseEnabled(enabled);
    found->second->setAlphaValue(enabled ? 1.f : 0.38f);
    enableChildren(found->second, enabled, getFrame());
}

void TelephonyEditor::setGroupVisible(const char* name, bool visible)
{
    const auto found = groups.find(name);
    if (found == groups.end())
        return;
    setGroupEnabled(name, visible);
    // Hide each descendant as well: VSTGUI's focus traversal enters containers
    // even when their parent is not visible. No inaccessible codec can be tabbed
    // to in a distribution build.
    const auto setVisible = [&](auto&& self, VSTGUI::CView* view) -> void {
        view->setVisible(visible);
        if (auto* container = view->asViewContainer())
            container->forEachChild([&](VSTGUI::CView* child) { self(self, child); });
    };
    setVisible(setVisible, found->second);
}

void TelephonyEditor::refresh()
{
    if (refreshing || !getFrame())
        return;
    refreshing = true;
    auto* controller = getController();
    const auto input = choice(controller, kParamEraMode);
    const auto output = choice(controller, kParamOutputEndpoint);
    const auto uses = [&](int endpoint) { return input == endpoint || output == endpoint; };
    const bool bypass = controller->getParamNormalized(kParamMasterBypass) >= 0.5;
    const bool artifacts = controller->getParamNormalized(kParamArtifactsEnabled) >= 0.5;
    const bool scVbr = controller->getParamNormalized(kParamEvsScVbr) >= 0.5;
    const int segment = choice(controller, kParamDegradationSegment);

    setText("input-codec", codecForEndpoint(input));
    setText("output-codec", codecForEndpoint(output));
    setText("bypass", bypass ? "BYPASS ON" : "BYPASS OFF");
    setText("artifacts", artifacts ? "Artifacts: On" : "Artifacts: Off");
    setText("sc-vbr", scVbr ? "Force VBR: On" : "Force VBR: Off");
    setText("route-status", bypass ? "BYPASSED / latency-compensated dry" :
                                   "IN-PROCESS / two independent legs");
    setText("input-target", bypass ? "BYPASSED" :
        (segment == 0 || segment == 1 ? "DEGRADATION TARGET" : "NO ADDED LOSS"));
    setText("output-target", bypass ? "BYPASSED" :
        (segment == 0 || segment == 2 ? "DEGRADATION TARGET" : "NO ADDED LOSS"));
    setText("network-note", segment == 3 ? "No leg selected: loss and degradation are inactive." :
                                         "Configured loss per selected leg. This is not a live meter.");
    setGroupEnabled("network-controls", segment != 3);
    setGroupEnabled("artifact-amount", artifacts);
    setGroupEnabled("g711", uses(0));
#ifdef TELEPHONY_DISTRIBUTION_BUILD
    setGroupVisible("distribution-note", true);
    setGroupVisible("distribution-bandwidth-note", true);
    setGroupVisible("amr-nb", false);
    setGroupVisible("amr-wb", false);
    setGroupVisible("evs-rate", false);
    setGroupVisible("evs-bitrate", false);
    setGroupVisible("evs-bandwidth", false);
    setGroupVisible("evs-options", false);
    setText("codec-note", "Distribution build / G.711, GSM and 5G approximation");
    setText("build-note", "DISTRIBUTION BUILD / reference codecs excluded");
#else
    setGroupVisible("distribution-note", false);
    setGroupVisible("distribution-bandwidth-note", false);
    setGroupEnabled("amr-nb", uses(2));
    setGroupEnabled("amr-wb", uses(3));
    const bool usesEvs = uses(5)
#if TELEPHONY_USE_EVS_JBM
        || uses(6)
#endif
        ;
    setGroupEnabled("evs-rate", usesEvs);
    setGroupEnabled("evs-bitrate", usesEvs);
    setGroupEnabled("evs-bandwidth", usesEvs);
    setGroupEnabled("evs-options", usesEvs);
    const bool cappedEvs = usesEvs && evsBitrateIsCapped(controller);
    const auto note = namedViews.find("codec-note");
    if (note != namedViews.end()) {
        if (auto* label = dynamic_cast<VSTGUI::CTextLabel*>(note->second)) {
            VSTGUI::CColor color;
            if (getUIDescription()->getColor(cappedEvs ? "Amber" : "Muted", color))
                label->setFontColor(color);
        }
    }
    if (cappedEvs && scVbr)
        setText("codec-note", "NB fixed-rate request capped at 24.4 kbps. Force VBR currently selects 5.9 kbps.");
    else if (cappedEvs)
        setText("codec-note", "NB bitrate capped at 24.4 kbps. Controls show the requested settings.");
    else if (usesEvs && scVbr)
        setText("codec-note", "SC-VBR forced (NB/WB). Turn Force VBR off to use the selected bitrate.");
    else if (usesEvs && choice(controller, kParamEvsBitrate) == 0)
        setText("codec-note", "5.9 kbps always uses SC-VBR (NB/WB), even with Force VBR off.");
    else if (usesEvs)
        setText("codec-note", "EVS bandwidth is capped by sample rate; NB is limited to 24.4 kbps.");
    else
        setText("codec-note", "Only the codecs on this route are active.");
    setText("build-note", "PERSONAL BUILD / contains reference codecs");
#endif
    refreshing = false;
}

IPlugView* PLUGIN_API TelephonyVoiceController::createView(FIDString name)
{
    if (name && std::strcmp(name, ViewType::kEditor) == 0) {
        auto* editor = new TelephonyEditor(this);
        if (editor->getUIDescription()->getViewAttributes("view"))
            return editor;
        // A damaged or incomplete bundle can fall back to the host's generic
        // editor rather than opening an empty custom window.
        editor->release();
    }
    return nullptr;
}

} // namespace Steinberg::Vst
