// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/ui.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cell::ui {
struct TimelinePeak { float low{}, high{}; };
struct TimelineClip {
    double begin{}, duration{};
    std::shared_ptr<const std::vector<TimelinePeak>> peaks;
};
struct TimelineTrack {
    std::string id, name;
    std::uint32_t color=0xffa8d989;
    std::vector<TimelineClip> clips;
    bool pending{};
};
struct TimelineSection { double begin{},end{};std::string name;std::uint32_t color=0xffa8d989; };
// Owning immutable display data, prepared off audio. The renderer never opens
// media or reads source samples. Track count follows this actual collection.
struct TimelineScene {
    std::vector<TimelineTrack> tracks;
    std::vector<TimelineSection> sections;
    double begin{},end=8.;
};
struct TimelineViewport { double begin{},end=8.,firstTrack{},trackCount=8.; };
struct TimelineNavigation {
    TimelineViewport view;
    std::optional<double> seek;
};
struct TimelineFrameStats { std::uint64_t baseBuilds{},paints{};std::size_t tracks{}; };

// Complete overview interaction/rendering surface, independent of any host or
// legacy widget framework. The same editor runs embedded or in a native window.
// Static data/labels and moving overlays have independent invalidation; idle
// ticks and cursor motion never rescan PCM or rebuild the waveform background.
class TimelineOverview final : public Editor {
  public:
    TimelineOverview();
    ~TimelineOverview() override;
    void setScene(std::shared_ptr<const TimelineScene>);
    void setViewport(TimelineViewport);
    TimelineViewport viewport() const noexcept;
    void setCursor(double);
    void setSelection(std::optional<std::pair<double,double>>);
    void setLoop(std::optional<std::pair<double,double>>);
    void setBackingScale(double);
    void setEdited(bool);
    bool edited() const noexcept;
    bool cancelGesture();
    Rect plotBounds() const noexcept;
    Rect viewportBounds() const noexcept;
    TimelineFrameStats stats() const noexcept;
    void paint(Canvas&,Rect) override;
    bool pointer(const PointerEvent&) override;
    bool keyboard(const KeyEvent&) override;
    bool tick() override;
    void cancelInput() override;
    const char* title()const noexcept override{return "Band Aide — Timeline";}
    Point preferredSize()const noexcept override{return {1200,600};}
    bool preserveAspectRatio()const noexcept override{return false;}
    Point constrainSize(Point p)const noexcept override{return {std::max(320.,p.x),std::max(140.,p.y)};}
    std::function<void(const TimelineNavigation&)> navigate;
    std::function<void()> close;
    std::function<void(bool)> editedChanged;
    std::function<void(Point)> contextMenu;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
