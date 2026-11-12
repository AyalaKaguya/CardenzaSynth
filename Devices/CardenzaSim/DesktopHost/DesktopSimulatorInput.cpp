#include "DesktopSimulator.h"

#include "Devices/CardenzaSim/SimulatorInput.h"
#include "Library/Synth/Core/SynthTypes.h"
#include "OS/System/System.h"

namespace CardenzaSim::DesktopHost {

namespace {

std::size_t MatrixIndex(MatrixPosition position)
{
  return position.row * kKeyboardColumns + position.column;
}

bool IsModifierVirtualKey(UINT virtualKey)
{
  return virtualKey == VK_CONTROL || virtualKey == VK_LCONTROL || virtualKey == VK_RCONTROL ||
         virtualKey == VK_SHIFT || virtualKey == VK_LSHIFT || virtualKey == VK_RSHIFT ||
         virtualKey == VK_MENU || virtualKey == VK_LMENU || virtualKey == VK_RMENU;
}

}  // namespace

void DesktopSimulator::OnLeftButtonDown(int x, int y)
{
  const POINT point{x, y};
  UpdateLayout();
#if defined(CARDENZA_NATIVE_UI)
  if (PointInRect(screenRect_, point))
  {
    touchCaptured_ = true;
    lastTouch_ = {static_cast<LONG>((x - screenRect_.left) / scale_), static_cast<LONG>((y - screenRect_.top) / scale_)};
    Input::SendTouch({Device::Touch::Kind::Down, 0, static_cast<std::int16_t>(lastTouch_.x), static_cast<std::int16_t>(lastTouch_.y)});
    SetCapture(hwnd_);
    return;
  }
#endif

  for (std::size_t index = 0; index < scaleButtons_.size(); ++index)
  {
    if (PointInRect(scaleButtons_[index], point))
    {
      const int nextScale = index == 0 ? 1 : (index == 1 ? 2 : 4);
      if (nextScale != scale_)
      {
        scale_ = nextScale;
        ResizeForScale();
      }
      return;
    }
  }

  if (PointInRect(homeButton_, point))
  {
    TapHomeButton();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return;
  }

  for (std::size_t index = 0; index < keyButtons_.size(); ++index)
  {
    if (PointInRect(keyButtons_[index], point))
    {
      const auto position = PositionForIndex(index);
      if (IsControlModifierDown())
      {
        ToggleVirtualKey(position);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
      }

      const bool shouldCapture = ShouldCaptureKey(position);
      SetVirtualKey(position, true);
      if (shouldCapture)
      {
        SetCapture(hwnd_);
        capturedKey_ = static_cast<int>(index);
      }
      else
      {
        SetVirtualKey(position, false);
      }
      InvalidateRect(hwnd_, nullptr, FALSE);
      return;
    }
  }
}

void DesktopSimulator::OnMouseMove(int x, int y, UINT flags)
{
#if defined(CARDENZA_NATIVE_UI)
  if (touchCaptured_)
  {
    lastTouch_ = {static_cast<LONG>((x - screenRect_.left) / scale_), static_cast<LONG>((y - screenRect_.top) / scale_)};
    Input::SendTouch({Device::Touch::Kind::Move, 0, static_cast<std::int16_t>(lastTouch_.x), static_cast<std::int16_t>(lastTouch_.y)});
    return;
  }
#endif
  if ((flags & MK_LBUTTON) == 0 || capturedKey_ < 0)
  {
    return;
  }

  const auto capturedPosition = PositionForIndex(static_cast<std::size_t>(capturedKey_));
  if (capturedPosition.column != synth::kHomeLeftBarColumn)
  {
    return;
  }

  const POINT point{x, y};
  for (std::size_t index = 0; index < keyButtons_.size(); ++index)
  {
    if (!PointInRect(keyButtons_[index], point))
    {
      continue;
    }

    if (dragMenuIndex_ == static_cast<int>(index))
    {
      return;
    }

    const auto position = PositionForIndex(index);
    if (!IsMenuKey(position.row, position.column))
    {
      dragMenuIndex_ = -1;
      return;
    }

    TapVirtualKey(position);
    dragMenuIndex_ = static_cast<int>(index);
    InvalidateRect(hwnd_, nullptr, FALSE);
    return;
  }

  dragMenuIndex_ = -1;
}

void DesktopSimulator::OnLeftButtonUp()
{
#if defined(CARDENZA_NATIVE_UI)
  if (touchCaptured_)
  {
    touchCaptured_ = false;
    Input::SendTouch({Device::Touch::Kind::Up, 0, static_cast<std::int16_t>(lastTouch_.x), static_cast<std::int16_t>(lastTouch_.y)});
    ReleaseCapture();
    return;
  }
#endif
  if (capturedKey_ < 0)
  {
    return;
  }

  const auto position = PositionForIndex(static_cast<std::size_t>(capturedKey_));
  ReleaseVirtualKey(position);

  capturedKey_ = -1;
  dragMenuIndex_ = -1;
  ReleaseCapture();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void DesktopSimulator::OnKeyDown(UINT virtualKey, LPARAM keyInfo)
{
  const bool wasAlreadyDown = (keyInfo & (1 << 30)) != 0;
  if (IsControlModifierDown() && !IsModifierVirtualKey(virtualKey))
  {
    if (const auto position = ResolveVirtualKey(virtualKey))
    {
      if (wasAlreadyDown)
      {
        return;
      }
      ToggleVirtualKey(*position);
      InvalidateRect(hwnd_, nullptr, FALSE);
      return;
    }
  }

  if (virtualKey == '1' || virtualKey == '2' || virtualKey == '4')
  {
    const int nextScale = virtualKey == '1' ? 1 : (virtualKey == '2' ? 2 : 4);
    if (nextScale != scale_)
    {
      scale_ = nextScale;
      ResizeForScale();
    }
    return;
  }

  if (virtualKey == VK_HOME)
  {
    TapHomeButton();
    InvalidateRect(hwnd_, nullptr, FALSE);
  }
}

void DesktopSimulator::TapHomeButton()
{
  CardenzaSim::Input::SetHomeButton(true);
  os::system::RunFrame();
  CardenzaSim::Input::SetHomeButton(false);
  os::system::RunFrame();
}

bool DesktopSimulator::IsControlModifierDown() const
{
  return (GetKeyState(VK_CONTROL) & 0x8000) != 0;
}

std::optional<MatrixPosition> DesktopSimulator::ResolveVirtualKey(UINT virtualKey) const
{
  switch (virtualKey)
  {
    case VK_ESCAPE:
      return MatrixPosition{0, 0};
    case VK_TAB:
      return MatrixPosition{1, 0};
    case VK_CAPITAL:
      return MatrixPosition{2, 1};
    case VK_RETURN:
      return MatrixPosition{2, 13};
    case VK_SPACE:
      return MatrixPosition{3, 13};
    case VK_BACK:
    case VK_DELETE:
      return MatrixPosition{0, 13};
    case VK_OEM_MINUS:
      return MatrixPosition{0, 11};
    case VK_OEM_PLUS:
      return MatrixPosition{0, 12};
    case VK_OEM_4:
      return MatrixPosition{1, 11};
    case VK_OEM_6:
      return MatrixPosition{1, 12};
    case VK_OEM_5:
      return MatrixPosition{1, 13};
    case VK_OEM_1:
      return MatrixPosition{2, 11};
    case VK_OEM_7:
      return MatrixPosition{2, 12};
    case VK_OEM_COMMA:
      return MatrixPosition{3, 10};
    case VK_OEM_PERIOD:
      return MatrixPosition{3, 11};
    case VK_OEM_2:
      return MatrixPosition{3, 12};
    default:
      break;
  }

  if (virtualKey >= '1' && virtualKey <= '9')
  {
    return MatrixPosition{0, static_cast<std::size_t>(virtualKey - '0')};
  }
  if (virtualKey == '0')
  {
    return MatrixPosition{0, 10};
  }
  {
    static constexpr std::array<char, 10> kRow1Keys = {'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P'};
    for (std::size_t index = 0; index < kRow1Keys.size(); ++index)
    {
      if (virtualKey == kRow1Keys[index])
      {
        return MatrixPosition{1, index + 1};
      }
    }
  }
  {
    static constexpr std::array<char, 9> kRow2Keys = {'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L'};
    for (std::size_t index = 0; index < kRow2Keys.size(); ++index)
    {
      if (virtualKey == kRow2Keys[index])
      {
        return MatrixPosition{2, index + 2};
      }
    }
  }
  {
    static constexpr std::array<char, 7> kRow3Keys = {'Z', 'X', 'C', 'V', 'B', 'N', 'M'};
    for (std::size_t index = 0; index < kRow3Keys.size(); ++index)
    {
      if (virtualKey == kRow3Keys[index])
      {
        return MatrixPosition{3, index + 3};
      }
    }
  }

  return std::nullopt;
}

bool DesktopSimulator::ShouldCaptureKey(MatrixPosition position) const
{
  return position.column == synth::kHomeLeftBarColumn ||
         position.column == synth::kHomeRightBarColumn ||
         IsMenuKey(position.row, position.column) ||
         synth::IsSequencerStepKey(position.row, position.column) ||
         (position.column >= synth::kHomePerformanceFirstColumn &&
          position.column <= synth::kHomePerformanceLastColumn);
}

void DesktopSimulator::ApplyVirtualKey(MatrixPosition position)
{
  const std::size_t index = MatrixIndex(position);
  if (index >= momentaryKeys_.size())
  {
    return;
  }
  CardenzaSim::Input::SetMatrixKey(position.row, position.column, momentaryKeys_[index] || toggledKeys_[index]);
  os::system::RunFrame();
}

void DesktopSimulator::ToggleVirtualKey(MatrixPosition position)
{
  const std::size_t index = MatrixIndex(position);
  if (index >= toggledKeys_.size())
  {
    return;
  }
  toggledKeys_[index] = !toggledKeys_[index];
  ApplyVirtualKey(position);
}

void DesktopSimulator::SetVirtualKey(MatrixPosition position, bool pressed)
{
  const std::size_t index = MatrixIndex(position);
  if (index >= momentaryKeys_.size())
  {
    return;
  }
  momentaryKeys_[index] = pressed;
  ApplyVirtualKey(position);
}

void DesktopSimulator::ReleaseVirtualKey(MatrixPosition position)
{
  const std::size_t index = MatrixIndex(position);
  if (index >= momentaryKeys_.size())
  {
    return;
  }
  momentaryKeys_[index] = false;
  toggledKeys_[index] = false;
  ApplyVirtualKey(position);
}

void DesktopSimulator::TapVirtualKey(MatrixPosition position)
{
  SetVirtualKey(position, true);
  SetVirtualKey(position, false);
}

}  // namespace CardenzaSim::DesktopHost
