#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "Applications/Synth/UI/Groove/GrooveController.h"
#include "Applications/Synth/UI/Groove/Native/NativeAction.h"
#include "Applications/Synth/UI/Groove/Render/Animator.h"
#include "Applications/Synth/UI/Groove/Render/Gfx.h"
#include "Devices/Device.h"

namespace groove::native {

// UI and touch state are owned by the system task. Rendering and hit testing use
// the same rectangles; a pointer never writes a parameter by its coordinates.
class View {
public:
  void Render(Gfx& g, Controller& controller, Animator& animator);
  bool Pointer(Controller& controller, const Device::Touch::Event& event, std::uint32_t nowMs);
  void Tick(Controller& controller, std::uint32_t nowMs);
  void Cancel(Controller& controller, std::uint32_t nowMs);

private:
  enum class Layer : std::uint8_t { None, Scenes, Tracks, Menu, Pages };
  struct Hit {
    Rect rect{};
    Command command{};
    Layer layer = Layer::None;
    bool side = false;
    bool localLayer = false;
  };
  struct PerformanceHit { Rect rect{}; int note = -1; };
  struct PerformancePointer {
    bool active = false;
    bool cancelled = false;
    std::uint8_t id = 0;
    int note = -1;
    std::uint32_t generation = 0;
  };
  static constexpr std::size_t kHitCapacity = 96;
  void AddHit(const Rect& rect, Command command, bool side = false);
  void AddLayerHit(const Rect& rect, Layer layer);
  void AddPerformanceHit(const Rect& rect, int note);
  int PerformanceNoteAt(int x, int y) const;
  void CancelPerformance(Controller& controller, std::uint32_t nowMs, bool clear = false);
  void Header(Gfx& g, const Controller& controller, Animator& animator);
  void Sides(Gfx& g, const Controller& controller, Animator& animator);
  void Tabs(Gfx& g, const Controller& controller, Animator& animator);
  void Layers(Gfx& g, const Controller& controller, Animator& animator, Layer layer, float progress);
  void SetLayer(Controller& controller, Layer layer, std::uint32_t nowMs);
  void SceneContent(Gfx& g, const Controller& controller, Animator& animator, Scene scene, std::uint8_t page);
  void ModalContent(Gfx& g, const Controller& controller, Animator& animator, const Modal& modal);
  void SlotPage(Gfx& g, const Controller& controller, Animator& animator,
                const InstrumentPage& page, std::uint8_t focus, bool current = true);
  void Graphic(Gfx& g, const Controller& controller, Animator& animator,
               const InstrumentPage& page, const Rect& area);
  void Sequence(Gfx& g, const Controller& controller, Animator& animator, std::uint8_t page);
  void PianoRoll(Gfx& g, const Controller& controller, Animator& animator);
  void Tape(Gfx& g, const Controller& controller, Animator& animator, std::uint8_t page);
  void Setup(Gfx& g, const Controller& controller, Animator& animator, std::uint8_t page);
  void Cell(Gfx& g, const Rect& rect, std::string_view label, std::string_view value,
            float fraction, bool focused, Color accent, bool available = true);
  void Action(Gfx& g, const Rect& rect, std::string_view label, std::string_view detail,
              bool focused, Color accent, bool available = true);
  static std::uint32_t Generation(const Controller& controller);
  void EndPointer(Controller& controller, std::uint32_t nowMs);

  std::array<Hit, kHitCapacity> hits_{};
  std::size_t hitCount_ = 0;
  std::array<PerformanceHit, 64> performanceHits_{};
  std::size_t performanceHitCount_ = 0;
  std::array<PerformancePointer, 2> performancePointers_{};
  int rollLowNote_ = 48;
  Layer layer_ = Layer::None;
  Hit pressed_{};
  std::uint8_t pointerId_ = 0;
  bool pointerDown_ = false;
  bool pointerCancelled_ = false;
  std::uint32_t pressedAt_ = 0;
  bool holdOpened_ = false;
  std::uint32_t pressGeneration_ = 0;
  std::uint32_t renderGeneration_ = 0;
  std::uint32_t layerOpenedAt_ = 0;
  std::uint32_t layerClosedAt_ = 0;
  Layer closingLayer_ = Layer::None;
  bool registerHits_ = true;
};

}  // namespace groove::native
