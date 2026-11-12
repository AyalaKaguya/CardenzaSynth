#include "DesktopSimulator.h"

#include <filesystem>
#include <string>
#include <utility>
#include <windowsx.h>

#include "Applications/Application.h"
#if defined(CARDENZA_GROOVE_UI)
#include "Applications/Synth/GrooveApp.h"
#else
#include "Applications/Synth/SynthApp.h"
#endif
#include "Devices/CardenzaSim/DesktopHost/DesktopSimulatorDebug.h"
#include "OS/MIDI/MIDI.h"
#include "OS/System/System.h"

namespace CardenzaSim::DesktopHost {

DesktopSimulator::DesktopSimulator(int initialScale, DebugScriptOptions debugOptions)
  : scale_(initialScale), debugOptions_(std::move(debugOptions)), keyboard_(BuildKeyboard()) {}

DesktopSimulator::~DesktopSimulator() = default;

bool DesktopSimulator::Initialize()
{
  os::midi::Initialize();
  if (!os::system::Begin())
  {
    MessageBoxA(nullptr, "Failed to create Synth application.", "CardenzaSimDesktop", MB_ICONERROR | MB_OK);
    return false;
  }

  app::Application* application = os::system::ActiveApplication();
  synthApp_ = dynamic_cast<apps::SimulatedApp*>(application);
  if (synthApp_ == nullptr)
  {
    MessageBoxA(nullptr, "Synth application registry returned an unexpected type.", "CardenzaSimDesktop", MB_ICONERROR | MB_OK);
    return false;
  }

  if (!debugOptions_.scriptPath.empty())
  {
    debugOptions_.scriptPath = std::filesystem::absolute(std::filesystem::path(debugOptions_.scriptPath)).string();
    if (!debugOptions_.outputDirectory.empty())
    {
      std::filesystem::path outputPath(debugOptions_.outputDirectory);
      if (outputPath.is_relative())
      {
        outputPath = std::filesystem::path(debugOptions_.scriptPath).parent_path() / outputPath;
      }
      debugOptions_.outputDirectory = std::filesystem::absolute(outputPath).string();
    }

    debugSession_ = std::make_unique<DesktopDebugSession>(debugOptions_);
    std::string errorMessage;
    if (!debugSession_->Load(errorMessage))
    {
      MessageBoxA(nullptr, errorMessage.c_str(), "CardenzaSimDesktop", MB_ICONERROR | MB_OK);
      return false;
    }
  }

  return true;
}

bool DesktopSimulator::Create(HINSTANCE instance)
{
  WNDCLASSA windowClass{};
  windowClass.lpfnWndProc = &DesktopSimulator::WndProc;
  windowClass.hInstance = instance;
  windowClass.lpszClassName = "CardenzaSimDesktopWindow";
  windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
  windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  RegisterClassA(&windowClass);

  RECT clientRect = ComputeClientRect();
  RECT windowRect = clientRect;
  AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX, FALSE);

  hwnd_ = CreateWindowExA(
      0,
      windowClass.lpszClassName,
      "CardenzaSim Desktop",
      WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      windowRect.right - windowRect.left,
      windowRect.bottom - windowRect.top,
      nullptr,
      nullptr,
      instance,
      this);

  if (hwnd_ != nullptr)
  {
    lastFrameTick_ = GetTickCount64();
    SetTimer(hwnd_, kFrameTimerId, kFrameTimerPeriodMs, nullptr);
  }
  return hwnd_ != nullptr;
}

int DesktopSimulator::Run()
{
  ShowWindow(hwnd_, debugOptions_.hideWindow ? SW_HIDE : SW_SHOWDEFAULT);
  UpdateWindow(hwnd_);

  MSG message{};
  while (GetMessageA(&message, nullptr, 0, 0) > 0)
  {
    TranslateMessage(&message);
    DispatchMessageA(&message);
  }
  return static_cast<int>(message.wParam);
}

LRESULT CALLBACK DesktopSimulator::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  auto* simulator = reinterpret_cast<DesktopSimulator*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
  if (message == WM_NCCREATE)
  {
    const auto* create = reinterpret_cast<CREATESTRUCTA*>(lParam);
    simulator = reinterpret_cast<DesktopSimulator*>(create->lpCreateParams);
    simulator->hwnd_ = hwnd;
    SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(simulator));
  }

  if (!simulator)
  {
    return DefWindowProcA(hwnd, message, wParam, lParam);
  }

  return simulator->HandleMessage(message, wParam, lParam);
}

LRESULT DesktopSimulator::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
  switch (message)
  {
#if defined(CARDENZA_NATIVE_UI)
    case WM_KILLFOCUS:
      Device::Touch::CancelAll();
      touchCaptured_ = false;
      return 0;
    case WM_CAPTURECHANGED:
      if (touchCaptured_)
      {
        touchCaptured_ = false;
        Device::Touch::CancelAll();
      }
      return 0;
#endif
    case WM_PAINT:
      OnPaint();
      return 0;
    case WM_LBUTTONDOWN:
      OnLeftButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
      return 0;
    case WM_MOUSEMOVE:
      OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), static_cast<UINT>(wParam));
      return 0;
    case WM_LBUTTONUP:
      OnLeftButtonUp();
      return 0;
    case WM_KEYDOWN:
      OnKeyDown(static_cast<UINT>(wParam), lParam);
      return 0;
    case WM_TIMER:
      if (wParam == kFrameTimerId)
      {
        OnFrameTick();
        return 0;
      }
      return 0;
    case WM_DESTROY:
      KillTimer(hwnd_, kFrameTimerId);
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcA(hwnd_, message, wParam, lParam);
  }
}

void DesktopSimulator::OnFrameTick()
{
  lastFrameTick_ = GetTickCount64();

  os::system::RunFrame();

  if (debugSession_)
  {
    std::string errorMessage;
    if (!debugSession_->Step(*this, errorMessage))
    {
      MessageBoxA(hwnd_, errorMessage.c_str(), "CardenzaSimDesktop Debug", MB_ICONERROR | MB_OK);
      PostMessageA(hwnd_, WM_CLOSE, 0, 0);
      return;
    }

    if (debugSession_->IsComplete() && debugSession_->ExitOnComplete())
    {
      PostMessageA(hwnd_, WM_CLOSE, 0, 0);
      return;
    }
  }

  InvalidateRect(hwnd_, nullptr, FALSE);
}

}  // namespace CardenzaSim::DesktopHost
