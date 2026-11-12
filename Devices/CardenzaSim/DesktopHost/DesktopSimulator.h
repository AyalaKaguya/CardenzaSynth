#pragma once

#define NOMINMAX
#include <windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "DesktopHostLayout.h"

namespace apps {
#if defined(CARDENZA_GROOVE_UI)
class GrooveApp;
using SimulatedApp = GrooveApp;
#else
class SynthApp;
using SimulatedApp = SynthApp;
#endif
}

namespace CardenzaSim::DesktopHost {

struct DebugScriptOptions {
  std::string scriptPath;
  std::string outputDirectory;
  bool exitOnComplete = false;
  bool hideWindow = false;
};

class DesktopDebugSession;

inline constexpr UINT_PTR kFrameTimerId = 1;
inline constexpr UINT kFrameTimerPeriodMs = 16;

class DesktopSimulator {
public:
  explicit DesktopSimulator(int initialScale, DebugScriptOptions debugOptions = {});
  ~DesktopSimulator();

  bool Initialize();
  bool Create(HINSTANCE instance);
  int Run();

private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

  LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
  RECT ComputeClientRect() const;
  void UpdateLayout();
  void ResizeForScale();
  int ScreenPx(int value) const;

  void OnFrameTick();
  void OnLeftButtonDown(int x, int y);
  void OnMouseMove(int x, int y, UINT flags);
  void OnLeftButtonUp();
  void OnKeyDown(UINT virtualKey, LPARAM keyInfo);

  void TapHomeButton();
  bool IsControlModifierDown() const;
  std::optional<MatrixPosition> ResolveVirtualKey(UINT virtualKey) const;
  bool ShouldCaptureKey(MatrixPosition position) const;
  void ApplyVirtualKey(MatrixPosition position);
  void ToggleVirtualKey(MatrixPosition position);
  void SetVirtualKey(MatrixPosition position, bool pressed);
  void ReleaseVirtualKey(MatrixPosition position);
  void TapVirtualKey(MatrixPosition position);

  std::string FooterText() const;
  std::string KeyboardHeaderText() const;
  std::size_t ContextMenuFirstColumn() const;
  std::size_t ContextMenuColumnIndex(std::size_t column) const;
  bool IsMenuKey(std::size_t row, std::size_t column) const;
  bool IsHomeRightBarKey(std::size_t column) const;

  COLORREF KeyFillColor(std::size_t index, bool active) const;
  COLORREF KeyEdgeColor(std::size_t index, bool active) const;
  COLORREF KeyLedColor(std::size_t index,
                       bool deviceLedsEnabled,
                       std::uint8_t deviceLedGlobalBrightness) const;
  void DrawFrameBuffer(HDC dc);
  void DrawScaleButtons(HDC dc);
  void DrawKeyboard(HDC dc);
  void DrawFooter(HDC dc);
  void OnPaint();
  std::optional<MatrixPosition> ResolveDebugKey(std::string_view token) const;
  std::filesystem::path ResolveDebugOutputPath(std::string_view path) const;
  bool SaveFrameBufferBmp(const std::filesystem::path& outputPath) const;

  friend class DesktopDebugSession;

  HWND hwnd_ = nullptr;
  int scale_ = kDefaultScale;
  int capturedKey_ = -1;
  int dragMenuIndex_ = -1;
#if defined(CARDENZA_NATIVE_UI)
  bool touchCaptured_ = false;
  POINT lastTouch_{};
#endif
  ULONGLONG lastFrameTick_ = 0;
  apps::SimulatedApp* synthApp_ = nullptr;
  DebugScriptOptions debugOptions_{};
  std::unique_ptr<DesktopDebugSession> debugSession_;
  std::array<KeyVisual, kKeyboardRows * kKeyboardColumns> keyboard_{};
  std::array<bool, kKeyboardRows * kKeyboardColumns> momentaryKeys_{};
  std::array<bool, kKeyboardRows * kKeyboardColumns> toggledKeys_{};
  std::array<RECT, 3> scaleButtons_{};
  RECT homeButton_{};
  std::array<RECT, kKeyboardRows * kKeyboardColumns> keyButtons_{};
  RECT screenRect_{};
  RECT keyboardHeaderRect_{};
  RECT footerRect_{};
};

}  // namespace CardenzaSim::DesktopHost
