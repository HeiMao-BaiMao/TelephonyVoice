#!/usr/bin/env python3
"""Dependency-free checks for the packaged VSTGUI description.

Runs without an SDK/window system; complements C++ compilation and a DAW smoke
check, rather than claiming that XML validation exercises native rendering.
"""
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
INTERACTIVE = {"COptionMenu", "CSlider", "CTextButton", "CTextEdit"}
EXPECTED = {
    "Input": "kParamInputRoute", "DryWet": "kParamDryWet", "Gain": "kParamOutputGain",
    "Artifacts": "kParamArtifactsEnabled", "ArtifactAmount": "kParamArtifactAmount",
    "Bypass": "kParamMasterBypass", "PacketLoss": "kParamPacketLossRate",
    "Degradation": "kParamNetworkDegradation", "Output": "kParamOutputRoute",
    "EvsRate": "kParamEvsSampleRate", "EvsBitrate": "kParamEvsBitrate",
    "EvsBandwidth": "kParamEvsMaxBw", "Segment": "kParamDegradationSegment",
    "AmrWb": "kParamAmrWbMode", "AmrNb": "kParamAmrNbMode", "G711Law": "kParamG711Law",
    "EvsSid": "kParamEvsDtxSidInterval", "EvsScVbr": "kParamEvsScVbr",
    "OpusBandwidth": "kParamOpusBandwidth", "OpusBitrate": "kParamOpusBitrate", "EditorPage": "kParamEditorPage",
}


def pair(value):
    return tuple(float(number.strip()) for number in value.split(","))


def validate():
    xml = ET.parse(ROOT / "resources/telephonyvoice.uidesc").getroot()
    assert xml.tag == "vstgui-ui-description"
    templates = xml.findall("template")
    assert len(templates) == 1 and templates[0].get("name") == "view"
    template = templates[0]
    assert pair(template.get("size")) == (940, 680)
    assert template.get("size") == template.get("minSize") == template.get("maxSize")

    # Resolve the C++ enum rather than trusting duplicated numeric IDs.
    header = (ROOT / "TelephonyVoice.h").read_text(encoding="utf-8")
    body = re.search(r"enum TelephonyParams\s*\{(.*?)\};", header, re.S).group(1)
    body = re.sub(r"//[^\n]*", "", body)
    enum, number = {}, -1
    for item in body.split(","):
        item = item.strip()
        if not item:
            continue
        name, *value = item.split("=")
        number = int(value[0].strip(), 0) if value else number + 1
        enum[name.strip()] = number
    tags = {tag.get("name"): int(tag.get("tag")) for tag in xml.findall("control-tags/control-tag")}
    descriptor_source = (ROOT / "dsp/AdvancedControls.h").read_text(encoding="utf-8")
    descriptor_keys = re.findall(r'\{\s*"([^"\n]+)",\s*"', descriptor_source)
    advanced_tags = {f"Advanced{index}": 200 + index for index in range(len(descriptor_keys))}
    assert set(tags) == set(EXPECTED) | set(advanced_tags), "All reachable basic and advanced controls must be bound"
    assert all(tags[name] == number for name, number in advanced_tags.items())
    assert len(set(tags.values())) == len(tags), "Duplicate parameter tags"
    for name, cpp_name in EXPECTED.items():
        assert tags[name] == enum[cpp_name], f"Parameter ID mismatch: {name}"

    colors = {node.get("name") for node in xml.findall("colors/color")}
    fonts = {node.get("name") for node in xml.findall("fonts/font")}
    gradients = {node.get("name") for node in xml.findall("gradients/gradient")}
    names, groups, bound, widgets = set(), set(), set(), []

    def walk(parent, absolute=(0, 0), page="common"):
        parent_w, parent_h = pair(parent.get("size"))
        for view in parent.findall("view"):
            cls = view.get("class")
            x, y = pair(view.get("origin"))
            w, h = pair(view.get("size"))
            assert x >= 0 and y >= 0 and w > 0 and h > 0
            assert x + w <= parent_w and y + h <= parent_h, f"View outside parent: {view.attrib}"

            for key, value in view.attrib.items():
                if "color" in key:
                    assert value in colors or value.startswith("~ ") or value.startswith("#"), (key, value)
                elif key == "font":
                    assert value in fonts, value
                elif key in {"gradient", "gradient-highlighted"}:
                    assert value in gradients, value
            if name := view.get("telephony-name"):
                assert name not in names, f"Duplicate presentation name: {name}"
                names.add(name)
            view_page = page
            if group := view.get("telephony-group"):
                assert group not in groups, f"Duplicate group name: {group}"
                groups.add(group)
                assert cls == "CViewContainer"
                if group == "basic-page" or group.startswith("advanced-page-"):
                    view_page = group
            if cls in INTERACTIVE:
                tag = view.get("control-tag")
                assert tag in tags, f"Unbound control: {view.attrib}"
                bound.add(tag)
                assert view.get("wants-focus") == "true", f"Keyboard focus missing: {tag}"
                assert view.get("tooltip"), f"Accessible explanatory text missing: {tag}"
                assert h >= 18, f"Control too small: {tag}"
                widgets.append((absolute[0] + x, absolute[1] + y, w, h, tag, view_page))
            if cls == "CViewContainer":
                walk(view, (absolute[0] + x, absolute[1] + y), view_page)
    walk(template)
    assert bound == set(tags), "Some parameter tags have no interactive view"
    for i, (x, y, w, h, name, page) in enumerate(widgets):
        for x2, y2, w2, h2, name2, page2 in widgets[i+1:]:
            if page != "common" and page2 != "common" and page != page2:
                continue
            assert x + w <= x2 or x2 + w2 <= x or y + h <= y2 or y2 + h2 <= y, \
                f"Interactive views overlap: {name}, {name2}"

    source = (ROOT / "TelephonyEditor.cpp").read_text(encoding="utf-8")
    processor_source = (ROOT / "TelephonyVoice.cpp").read_text(encoding="utf-8")
    def int_table(text, name):
        match = re.search(r"\b" + name + r"\[[^\]]*\]\s*=\s*\{([^}]+)\}", text, re.S)
        assert match, f"Missing EVS choice table: {name}"
        return [int(item.strip()) for item in match.group(1).split(",") if item.strip()]
    assert int_table(source, "sampleRates") == int_table(processor_source, "kEvsSampleRateValues")
    assert int_table(source, "bitrates") == int_table(processor_source, "kEvsBitrateValues")
    assert "TelephonyDSP::normalizeEvsConfig(" in source, "EVS cap warnings must use the shared DSP normalizer"
    assert "publishTelemetry(data" in processor_source and "data.outputParameterChanges" in processor_source
    assert set(re.findall(r'setText\("([^"]+)"', source)) <= names
    assert set(re.findall(r'setGroup(?:Enabled|Visible)\("([^"]+)"', source)) <= groups
    focus = xml.find("custom/attributes[@name='FocusDrawing']")
    assert focus is not None and focus.get("enabled") == "true"
    assert float(focus.get("width")) >= 2
    assert 'setAllowedZoomFactors({0.75, 1.0, 1.25, 1.5, 2.0})' in source
    print(f"Editor description OK: {len(tags)} parameter IDs, {len(widgets)} keyboard controls, bounded layout, named views, page separation and processor-backed telemetry")


if __name__ == "__main__":
    try:
        validate()
    except (AssertionError, ValueError, AttributeError, ET.ParseError) as error:
        print(f"Editor validation failed: {error}", file=sys.stderr)
        sys.exit(1)
