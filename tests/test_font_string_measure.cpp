// A font string measured against the text it holds now.
//
// The layout pass measures every string once a frame. Anything that reads a
// string's size before the next pass was answered for the *previous* text, and
// FrameXML lays two things out by hand in exactly that shape:
// WorldMapQuestFrame_UpdateQuests sizes each quest block from the objectives
// string it has just filled, and WatchFrame's quest handler measures the lines
// it has just written. A two-objective quest was given one line of room, so the
// next quest was drawn over the top of it; and the tracker's handler reported
// that it had used no pixels, which is the branch that collapses the tracker
// and disables its expand button.
//
// Needs an ImGui context and a built atlas, which is all sizeFontString needs
// and which costs nothing here - the default face is enough to tell one line
// from three.
#include <catch_amalgamated.hpp>

#include "ui/widget_renderer.hpp"
#include "ui/widget_tree.hpp"

#include <imgui.h>

using wowee::ui::sizeFontString;
using wowee::ui::Widget;
using wowee::ui::WidgetKind;

namespace {

/// One context for the run, with a real atlas behind it.
struct ImGuiFixture {
    ImGuiFixture() {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1920.0f, 1080.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.Fonts->AddFontDefault();
        io.Fonts->Build();
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        io.Fonts->SetTexID(static_cast<ImTextureID>(1));
        ImGui::NewFrame();
    }
    ~ImGuiFixture() { ImGui::DestroyContext(); }
};

ImGuiFixture& imgui() {
    static ImGuiFixture fixture;
    return fixture;
}

Widget fontString(const std::string& text) {
    Widget w;
    w.kind = WidgetKind::FontString;
    w.text = text;
    return w;
}

}  // namespace

TEST_CASE("a font string is as tall as the lines it holds", "[ui][fontstring]") {
    imgui();
    Widget one = fontString("one line");
    sizeFontString(one);
    const float oneLine = one.height;
    REQUIRE(oneLine > 0.0f);

    Widget three = fontString("one line\ntwo lines\nthree lines");
    sizeFontString(three);
    // Not a ratio to the decimal - the point is that three lines is about three
    // times one, and emphatically not one.
    REQUIRE(three.height > oneLine * 2.5f);
    REQUIRE(three.wrappedLines == 3);
}

TEST_CASE("changing the text changes the measurement", "[ui][fontstring]") {
    imgui();
    // The fault itself: measure, then write new text and measure again. The
    // second answer used to be the first one, because the cache key was the
    // text it had measured and nothing asked again until the next frame.
    Widget w = fontString("one line");
    sizeFontString(w);
    const float before = w.height;

    w.text = "one line\ntwo lines\nthree lines";
    sizeFontString(w);
    REQUIRE(w.height > before * 2.5f);
}

TEST_CASE("a re-measured string is put back in the resolve queue", "[ui][fontstring]") {
    imgui();
    // Half the fix. A getter reads the laid-out rect, and the resolve that
    // builds it is skipped for anything already marked done this generation -
    // so a string measured again mid-frame kept the rect built from its old
    // text however right the new measurement was.
    Widget w = fontString("one line");
    sizeFontString(w);
    w.resolvedGen = 7;

    // Same text: nothing moved, so nothing needs resolving again.
    sizeFontString(w);
    REQUIRE(w.resolvedGen == 7);

    w.text = "one line\ntwo lines\nthree lines";
    sizeFontString(w);
    REQUIRE(w.resolvedGen == 0);
}

TEST_CASE("measuring is cheap to ask for repeatedly", "[ui][fontstring]") {
    imgui();
    // It is called from every rect getter now, and those are read constantly.
    Widget w = fontString("some text");
    sizeFontString(w);
    const float height = w.height;
    for (int i = 0; i < 100; ++i) sizeFontString(w);
    REQUIRE(w.height == height);
}
