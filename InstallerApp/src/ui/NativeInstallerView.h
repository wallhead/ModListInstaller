#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace modlist {

struct NativeTheme {
  uint32_t background = 0x101416;
  uint32_t panel = 0x171D20;
  uint32_t panelAlt = 0x1D2428;
  uint32_t input = 0x111719;
  uint32_t line = 0x303A40;
  uint32_t lineStrong = 0x49565E;
  uint32_t text = 0xF3F5F1;
  uint32_t muted = 0xB7C0C4;
  uint32_t dim = 0x7F8B90;
  uint32_t accent = 0x9FC9DC;
  uint32_t accentStrong = 0xD7EEF7;
  uint32_t danger = 0x755052;
  std::wstring fontFamily = L"Segoe UI";
  float fontSize = 14.0f;
  float headerHeight = 78.0f;
  float footerHeight = 58.0f;
  float controlHeight = 32.0f;
  float cornerRadius = 3.0f;
  float contentPadding = 24.0f;
  float labelWidth = 150.0f;
};

struct NativeInstallerLayout {
  D2D1_RECT_F driveCombo{};
  D2D1_RECT_F installFrame{};
  D2D1_RECT_F installEdit{};
  D2D1_RECT_F browseButton{};
  D2D1_RECT_F finalPath{};
  D2D1_RECT_F statusArea{};
  D2D1_RECT_F progressBar{};
  D2D1_RECT_F logFrame{};
  D2D1_RECT_F logEdit{};
  D2D1_RECT_F openLogButton{};
  D2D1_RECT_F startButton{};
  D2D1_RECT_F stopButton{};
};

struct NativeInstallerViewState {
  std::wstring version;
  std::wstring unpackTarget;
  std::wstring finalInstallFolder;
  std::wstring status;
  int progress = 0;
};

enum class NativeButtonStyle {
  Normal,
  Input,
  Primary,
  Danger,
};

class NativeInstallerView {
public:
  bool Initialize(HWND hwnd, const std::filesystem::path& cssPath, std::wstring* warning);
  void Paint(HWND hwnd, const NativeInstallerViewState& state, const RECT* updateRect = nullptr);
  void PaintDialog(HWND hwnd, const std::wstring& title, const std::wstring& message);
  void PaintDrivePopup(HWND hwnd, const std::vector<std::wstring>& drives,
                       int selectedIndex, int hotIndex);
  void Resize(UINT width, UINT height);
  void DiscardDeviceResources();
  NativeInstallerLayout CalculateLayout(HWND hwnd) const;
  RECT ToPixels(HWND hwnd, const D2D1_RECT_F& rect) const;
  bool DrawButton(const DRAWITEMSTRUCT& item, NativeButtonStyle style, bool hot);
  bool DrawComboItem(const DRAWITEMSTRUCT& item);

  const NativeTheme& Theme() const { return theme_; }
  COLORREF TextColor() const;
  COLORREF MutedColor() const;
  COLORREF InputColor() const;

private:
  bool LoadTheme(const std::filesystem::path& cssPath, std::wstring* warning);
  HRESULT CreateTextFormats();
  HRESULT EnsureTarget(HWND hwnd);
  HRESULT EnsureDcTarget(HWND hwnd);
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> Brush(ID2D1RenderTarget* target,
                                                     uint32_t color) const;
  void DrawText(ID2D1RenderTarget* target,
                const std::wstring& value,
                const D2D1_RECT_F& rect,
                IDWriteTextFormat* format,
                ID2D1Brush* brush,
                DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING,
                bool wrap = false) const;

  NativeTheme theme_;
  Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory_;
  Microsoft::WRL::ComPtr<IDWriteFactory> writeFactory_;
  Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
  Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> dcTarget_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> titleFormat_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> versionFormat_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> bodyFormat_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> labelFormat_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> buttonFormat_;
};

}  // namespace modlist
