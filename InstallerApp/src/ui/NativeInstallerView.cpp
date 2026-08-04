#define NOMINMAX

#include "ui/NativeInstallerView.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace modlist {
namespace {

using Microsoft::WRL::ComPtr;

std::string Trim(std::string value) {
  const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
    return std::isspace(ch) != 0;
  });
  const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
    return std::isspace(ch) != 0;
  }).base();
  if (first >= last) {
    return {};
  }
  return std::string(first, last);
}

std::string RemoveComments(std::string_view css) {
  std::string result;
  result.reserve(css.size());
  bool inComment = false;
  for (size_t i = 0; i < css.size(); ++i) {
    if (!inComment && i + 1 < css.size() && css[i] == '/' && css[i + 1] == '*') {
      inComment = true;
      ++i;
      continue;
    }
    if (inComment && i + 1 < css.size() && css[i] == '*' && css[i + 1] == '/') {
      inComment = false;
      ++i;
      continue;
    }
    if (!inComment) {
      result.push_back(css[i]);
    }
  }
  return result;
}

std::unordered_map<std::string, std::string> ParseRootVariables(const std::string& css) {
  std::unordered_map<std::string, std::string> variables;
  const auto root = css.find(":root");
  if (root == std::string::npos) {
    return variables;
  }
  const auto open = css.find('{', root + 5);
  if (open == std::string::npos) {
    return variables;
  }

  size_t close = open + 1;
  int depth = 1;
  for (; close < css.size() && depth > 0; ++close) {
    if (css[close] == '{') ++depth;
    if (css[close] == '}') --depth;
  }
  if (depth != 0) {
    return variables;
  }

  const std::string_view body(css.data() + open + 1, close - open - 2);
  size_t position = 0;
  while (position < body.size()) {
    const size_t end = body.find(';', position);
    const std::string declaration = Trim(std::string(body.substr(
        position, end == std::string_view::npos ? body.size() - position : end - position)));
    const size_t colon = declaration.find(':');
    if (colon != std::string::npos) {
      std::string name = Trim(declaration.substr(0, colon));
      std::string value = Trim(declaration.substr(colon + 1));
      if (name.starts_with("--") && !value.empty()) {
        variables.insert_or_assign(std::move(name), std::move(value));
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    position = end + 1;
  }
  return variables;
}

std::optional<uint32_t> ParseHexColor(std::string value) {
  value = Trim(std::move(value));
  if (value.size() == 4 && value[0] == '#') {
    value = std::string{"#"} + value[1] + value[1] + value[2] + value[2] + value[3] + value[3];
  }
  if (value.size() != 7 || value[0] != '#') {
    return std::nullopt;
  }
  try {
    size_t parsed = 0;
    const auto color = std::stoul(value.substr(1), &parsed, 16);
    if (parsed != 6) {
      return std::nullopt;
    }
    return static_cast<uint32_t>(color);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<float> ParsePixels(std::string value) {
  value = Trim(std::move(value));
  if (value.ends_with("px")) {
    value.resize(value.size() - 2);
  }
  try {
    size_t parsed = 0;
    const float result = std::stof(value, &parsed);
    if (parsed != value.size() || !std::isfinite(result)) {
      return std::nullopt;
    }
    return result;
  } catch (...) {
    return std::nullopt;
  }
}

std::wstring Widen(std::string value) {
  value = Trim(std::move(value));
  const auto comma = value.find(',');
  if (comma != std::string::npos) {
    value.resize(comma);
  }
  value = Trim(std::move(value));
  if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                            (value.front() == '\'' && value.back() == '\''))) {
    value = value.substr(1, value.size() - 2);
  }
  if (value.empty()) {
    return {};
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                       nullptr, 0);
  std::wstring result(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      result.data(), size);
  return result;
}

D2D1_COLOR_F Color(uint32_t rgb) {
  return D2D1::ColorF(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                      static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                      static_cast<float>(rgb & 0xFF) / 255.0f);
}

COLORREF WinColor(uint32_t rgb) {
  return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

uint32_t Mix(uint32_t first, uint32_t second, float amount) {
  const auto channel = [amount](uint32_t a, uint32_t b) {
    const float firstValue = static_cast<float>(a);
    const float secondValue = static_cast<float>(b);
    return static_cast<uint32_t>(
        std::lround(firstValue + (secondValue - firstValue) * amount));
  };
  return (channel((first >> 16) & 0xFF, (second >> 16) & 0xFF) << 16) |
         (channel((first >> 8) & 0xFF, (second >> 8) & 0xFF) << 8) |
         channel(first & 0xFF, second & 0xFF);
}

D2D1_RECT_F Inset(D2D1_RECT_F rect, float amount) {
  rect.left += amount;
  rect.top += amount;
  rect.right -= amount;
  rect.bottom -= amount;
  return rect;
}

}  // namespace

bool NativeInstallerView::Initialize(HWND hwnd,
                                     const std::filesystem::path& cssPath,
                                     std::wstring* warning) {
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                               d2dFactory_.GetAddressOf()))) {
    return false;
  }
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                 __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(writeFactory_.GetAddressOf())))) {
    return false;
  }
  LoadTheme(cssPath, warning);
  if (FAILED(CreateTextFormats())) {
    return false;
  }
  return SUCCEEDED(EnsureTarget(hwnd));
}

bool NativeInstallerView::LoadTheme(const std::filesystem::path& cssPath,
                                    std::wstring* warning) {
  std::ifstream input(cssPath, std::ios::binary);
  if (!input) {
    if (warning != nullptr) {
      *warning = L"Файл темы не найден; используются встроенные цвета: " + cssPath.wstring();
    }
    return false;
  }
  const std::string css((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
  const auto variables = ParseRootVariables(RemoveComments(css));
  if (variables.empty()) {
    if (warning != nullptr) {
      *warning = L"В :root файла темы не найдены переменные; используются встроенные цвета.";
    }
    return false;
  }

  const auto setColor = [&variables](const char* name, uint32_t& target) {
    const auto found = variables.find(name);
    if (found != variables.end()) {
      if (const auto color = ParseHexColor(found->second); color.has_value()) {
        target = *color;
      }
    }
  };
  setColor("--bg", theme_.background);
  setColor("--panel", theme_.panel);
  setColor("--panel-2", theme_.panelAlt);
  setColor("--input", theme_.input);
  setColor("--line", theme_.line);
  setColor("--line-strong", theme_.lineStrong);
  setColor("--text", theme_.text);
  setColor("--muted", theme_.muted);
  setColor("--dim", theme_.dim);
  setColor("--accent", theme_.accent);
  setColor("--accent-strong", theme_.accentStrong);
  setColor("--danger", theme_.danger);

  const auto setPixels = [&variables](const char* name, float& target, float low, float high) {
    const auto found = variables.find(name);
    if (found != variables.end()) {
      if (const auto value = ParsePixels(found->second); value.has_value()) {
        target = std::clamp(*value, low, high);
      }
    }
  };
  setPixels("--font-size", theme_.fontSize, 11.0f, 20.0f);
  setPixels("--header-height", theme_.headerHeight, 64.0f, 120.0f);
  setPixels("--footer-height", theme_.footerHeight, 48.0f, 88.0f);
  setPixels("--control-height", theme_.controlHeight, 26.0f, 44.0f);
  setPixels("--corner-radius", theme_.cornerRadius, 0.0f, 8.0f);
  setPixels("--content-padding", theme_.contentPadding, 14.0f, 40.0f);
  setPixels("--label-width", theme_.labelWidth, 120.0f, 210.0f);

  if (const auto found = variables.find("--font-family"); found != variables.end()) {
    if (auto family = Widen(found->second); !family.empty()) {
      theme_.fontFamily = std::move(family);
    }
  }
  return true;
}

HRESULT NativeInstallerView::CreateTextFormats() {
  const auto create = [this](float size, DWRITE_FONT_WEIGHT weight,
                             ComPtr<IDWriteTextFormat>& target) {
    const HRESULT result = writeFactory_->CreateTextFormat(
        theme_.fontFamily.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, size, L"ru-RU", target.GetAddressOf());
    if (SUCCEEDED(result)) {
      target->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
      target->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return result;
  };
  HRESULT result = create(theme_.fontSize + 10.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, titleFormat_);
  if (FAILED(result)) return result;
  result = create(std::max(11.0f, theme_.fontSize - 2.0f), DWRITE_FONT_WEIGHT_NORMAL,
                  versionFormat_);
  if (FAILED(result)) return result;
  result = create(theme_.fontSize, DWRITE_FONT_WEIGHT_NORMAL, bodyFormat_);
  if (FAILED(result)) return result;
  result = create(std::max(12.0f, theme_.fontSize - 1.0f), DWRITE_FONT_WEIGHT_SEMI_BOLD,
                  labelFormat_);
  if (FAILED(result)) return result;
  return create(std::max(12.0f, theme_.fontSize - 1.0f), DWRITE_FONT_WEIGHT_SEMI_BOLD,
                buttonFormat_);
}

HRESULT NativeInstallerView::EnsureTarget(HWND hwnd) {
  if (target_) {
    return S_OK;
  }
  RECT client{};
  GetClientRect(hwnd, &client);
  const auto size = D2D1::SizeU(static_cast<UINT32>(std::max(1L, client.right)),
                               static_cast<UINT32>(std::max(1L, client.bottom)));
  const auto properties = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN));
  const auto hwndProperties = D2D1::HwndRenderTargetProperties(hwnd, size);
  const HRESULT result = d2dFactory_->CreateHwndRenderTarget(
      properties, hwndProperties, target_.GetAddressOf());
  if (SUCCEEDED(result)) {
    const float dpi = static_cast<float>(GetDpiForWindow(hwnd));
    target_->SetDpi(dpi, dpi);
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
  }
  return result;
}

HRESULT NativeInstallerView::EnsureDcTarget(HWND hwnd) {
  if (!dcTarget_) {
    const auto properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    const HRESULT result = d2dFactory_->CreateDCRenderTarget(&properties,
                                                             dcTarget_.GetAddressOf());
    if (FAILED(result)) {
      return result;
    }
  }
  const float dpi = static_cast<float>(GetDpiForWindow(hwnd));
  dcTarget_->SetDpi(dpi, dpi);
  dcTarget_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
  return S_OK;
}

ComPtr<ID2D1SolidColorBrush> NativeInstallerView::Brush(ID2D1RenderTarget* target,
                                                        uint32_t color) const {
  ComPtr<ID2D1SolidColorBrush> brush;
  target->CreateSolidColorBrush(Color(color), brush.GetAddressOf());
  return brush;
}

void NativeInstallerView::DrawText(ID2D1RenderTarget* target,
                                   const std::wstring& value,
                                   const D2D1_RECT_F& rect,
                                   IDWriteTextFormat* format,
                                   ID2D1Brush* brush,
                                   DWRITE_TEXT_ALIGNMENT alignment,
                                   bool wrap) const {
  format->SetTextAlignment(alignment);
  format->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
  target->DrawText(value.c_str(), static_cast<UINT32>(value.size()), format, rect, brush,
                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

NativeInstallerLayout NativeInstallerView::CalculateLayout(HWND hwnd) const {
  RECT client{};
  GetClientRect(hwnd, &client);
  const float dpi = static_cast<float>(GetDpiForWindow(hwnd));
  const float width = static_cast<float>(client.right) * 96.0f / dpi;
  const float height = static_cast<float>(client.bottom) * 96.0f / dpi;
  const float left = theme_.contentPadding;
  const float right = width - theme_.contentPadding;
  const float fieldLeft = left + theme_.labelWidth;
  const float noteTop = theme_.headerHeight + 16.0f;
  const float driveTop = noteTop + 48.0f;
  const float installTop = driveTop + theme_.controlHeight + 12.0f;
  const float finalTop = installTop + theme_.controlHeight + 12.0f;
  const float statusTop = finalTop + 50.0f;
  const float progressTop = statusTop + 30.0f;
  const float logTop = progressTop + 39.0f;
  const float footerTop = height - theme_.footerHeight;

  NativeInstallerLayout layout;
  layout.driveCombo = D2D1::RectF(fieldLeft, driveTop, fieldLeft + 126.0f,
                                  driveTop + theme_.controlHeight);
  layout.installFrame = D2D1::RectF(fieldLeft, installTop, right - 100.0f,
                                    installTop + theme_.controlHeight);
  layout.installEdit = Inset(layout.installFrame, 1.0f);
  layout.browseButton = D2D1::RectF(right - 92.0f, installTop, right,
                                    installTop + theme_.controlHeight);
  layout.finalPath = D2D1::RectF(left, finalTop, right, finalTop + 28.0f);
  layout.statusArea = D2D1::RectF(left, statusTop, right, statusTop + 23.0f);
  layout.progressBar = D2D1::RectF(left, progressTop, right, progressTop + 17.0f);
  layout.logFrame = D2D1::RectF(left, logTop, right,
                                std::max(logTop + 68.0f, footerTop - 14.0f));
  layout.logEdit = Inset(layout.logFrame, 2.0f);
  layout.openLogButton = D2D1::RectF(left, footerTop + 13.0f, left + 112.0f,
                                     height - 13.0f);
  layout.stopButton = D2D1::RectF(right - 104.0f, footerTop + 13.0f, right,
                                  height - 13.0f);
  layout.startButton = D2D1::RectF(layout.stopButton.left - 120.0f, footerTop + 13.0f,
                                   layout.stopButton.left - 8.0f, height - 13.0f);
  return layout;
}

RECT NativeInstallerView::ToPixels(HWND hwnd, const D2D1_RECT_F& rect) const {
  const float scale = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
  return RECT{static_cast<LONG>(std::lround(rect.left * scale)),
              static_cast<LONG>(std::lround(rect.top * scale)),
              static_cast<LONG>(std::lround(rect.right * scale)),
              static_cast<LONG>(std::lround(rect.bottom * scale))};
}

void NativeInstallerView::Paint(HWND hwnd, const NativeInstallerViewState& state,
                                const RECT* updateRect) {
  if (FAILED(EnsureTarget(hwnd))) {
    return;
  }
  const D2D1_SIZE_F size = target_->GetSize();
  const float width = size.width;
  const float height = size.height;
  const float left = theme_.contentPadding;
  const float right = width - theme_.contentPadding;
  const float fieldLeft = left + theme_.labelWidth;
  const float footerTop = height - theme_.footerHeight;
  const float noteTop = theme_.headerHeight + 16.0f;
  const auto layout = CalculateLayout(hwnd);

  const auto background = Brush(target_.Get(), theme_.background);
  const auto panel = Brush(target_.Get(), theme_.panel);
  const auto panelAlt = Brush(target_.Get(), theme_.panelAlt);
  const auto input = Brush(target_.Get(), theme_.input);
  const auto line = Brush(target_.Get(), theme_.lineStrong);
  const auto softLine = Brush(target_.Get(), theme_.line);
  const auto text = Brush(target_.Get(), theme_.text);
  const auto muted = Brush(target_.Get(), theme_.muted);
  const auto accent = Brush(target_.Get(), theme_.accent);

  target_->BeginDraw();
  bool clipped = false;
  if (updateRect != nullptr && updateRect->right > updateRect->left &&
      updateRect->bottom > updateRect->top) {
    const float dpi = static_cast<float>(GetDpiForWindow(hwnd));
    const float scale = 96.0f / dpi;
    target_->PushAxisAlignedClip(
        D2D1::RectF(static_cast<float>(updateRect->left) * scale,
                    static_cast<float>(updateRect->top) * scale,
                    static_cast<float>(updateRect->right) * scale,
                    static_cast<float>(updateRect->bottom) * scale),
        D2D1_ANTIALIAS_MODE_ALIASED);
    clipped = true;
  }
  target_->Clear(Color(theme_.background));
  target_->FillRectangle(D2D1::RectF(0, 0, width, height), background.Get());
  target_->FillRectangle(D2D1::RectF(0, theme_.headerHeight, width, footerTop), panel.Get());
  target_->FillRectangle(D2D1::RectF(0, 0, width, theme_.headerHeight), panelAlt.Get());
  target_->FillRectangle(D2D1::RectF(0, footerTop, width, height), panelAlt.Get());
  target_->DrawLine(D2D1::Point2F(0, theme_.headerHeight - 0.5f),
                    D2D1::Point2F(width, theme_.headerHeight - 0.5f), softLine.Get());
  target_->DrawLine(D2D1::Point2F(0, footerTop + 0.5f),
                    D2D1::Point2F(width, footerTop + 0.5f), softLine.Get());

  DrawText(target_.Get(), L"Modlist Installer Beta",
           D2D1::RectF(left, 11.0f, right, 43.0f), titleFormat_.Get(), text.Get());
  DrawText(target_.Get(), state.version,
           D2D1::RectF(left, 45.0f, right, theme_.headerHeight - 8.0f),
           versionFormat_.Get(), muted.Get());
  DrawText(target_.Get(),
           L"Распаковка должна происходить по короткому пути. После распаковки установщик перенесет все файлы в папку установки.",
           D2D1::RectF(left, noteTop, right, noteTop + 38.0f), bodyFormat_.Get(),
           muted.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, true);

  DrawText(target_.Get(), L"Диск для распаковки",
           D2D1::RectF(left, layout.driveCombo.top, fieldLeft - 12.0f,
                       layout.driveCombo.bottom),
           labelFormat_.Get(), text.Get());
  DrawText(target_.Get(), state.unpackTarget,
           D2D1::RectF(layout.driveCombo.right + 12.0f, layout.driveCombo.top,
                       right, layout.driveCombo.bottom),
           bodyFormat_.Get(), muted.Get());

  DrawText(target_.Get(), L"Папка установки",
           D2D1::RectF(left, layout.installFrame.top, fieldLeft - 12.0f,
                       layout.installFrame.bottom),
           labelFormat_.Get(), text.Get());
  target_->FillRoundedRectangle(
      D2D1::RoundedRect(layout.installFrame, theme_.cornerRadius, theme_.cornerRadius),
      input.Get());
  target_->DrawRoundedRectangle(
      D2D1::RoundedRect(layout.installFrame, theme_.cornerRadius, theme_.cornerRadius),
      line.Get());

  DrawText(target_.Get(), L"Итоговый путь",
           D2D1::RectF(left, layout.finalPath.top, fieldLeft - 12.0f,
                       layout.finalPath.bottom),
           labelFormat_.Get(), muted.Get());
  DrawText(target_.Get(), state.finalInstallFolder,
           D2D1::RectF(fieldLeft, layout.finalPath.top, right, layout.finalPath.bottom),
           bodyFormat_.Get(), muted.Get());

  DrawText(target_.Get(), state.status,
           D2D1::RectF(left, layout.statusArea.top, right - 60.0f,
                       layout.statusArea.bottom),
           bodyFormat_.Get(), muted.Get());
  DrawText(target_.Get(), std::to_wstring(std::clamp(state.progress, 0, 100)) + L"%",
           D2D1::RectF(right - 56.0f, layout.statusArea.top, right,
                       layout.statusArea.bottom),
           labelFormat_.Get(), accent.Get(), DWRITE_TEXT_ALIGNMENT_TRAILING);

  target_->FillRectangle(layout.progressBar, input.Get());
  target_->DrawRectangle(layout.progressBar, line.Get());
  const float fraction = static_cast<float>(std::clamp(state.progress, 0, 100)) / 100.0f;
  target_->FillRectangle(D2D1::RectF(
                             layout.progressBar.left + 1.0f,
                             layout.progressBar.top + 1.0f,
                             layout.progressBar.left +
                                 (layout.progressBar.right - layout.progressBar.left) * fraction,
                             layout.progressBar.bottom - 1.0f),
                          accent.Get());

  target_->FillRectangle(layout.logFrame, input.Get());
  target_->DrawRectangle(layout.logFrame, line.Get());

  if (clipped) {
    target_->PopAxisAlignedClip();
  }

  const HRESULT result = target_->EndDraw();
  if (result == D2DERR_RECREATE_TARGET) {
    DiscardDeviceResources();
  }
}

void NativeInstallerView::PaintDialog(HWND hwnd,
                                      const std::wstring& titleValue,
                                      const std::wstring& messageValue) {
  RECT client{};
  GetClientRect(hwnd, &client);
  ComPtr<ID2D1HwndRenderTarget> dialogTarget;
  const auto properties = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN));
  const auto hwndProperties = D2D1::HwndRenderTargetProperties(
      hwnd, D2D1::SizeU(static_cast<UINT32>(std::max(1L, client.right)),
                        static_cast<UINT32>(std::max(1L, client.bottom))));
  if (FAILED(d2dFactory_->CreateHwndRenderTarget(properties, hwndProperties,
                                                  dialogTarget.GetAddressOf()))) {
    return;
  }
  const float dpi = static_cast<float>(GetDpiForWindow(hwnd));
  dialogTarget->SetDpi(dpi, dpi);
  dialogTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
  const D2D1_SIZE_F size = dialogTarget->GetSize();
  const float footerTop = size.height - theme_.footerHeight;
  const auto panel = Brush(dialogTarget.Get(), theme_.panelAlt);
  const auto content = Brush(dialogTarget.Get(), theme_.panel);
  const auto line = Brush(dialogTarget.Get(), theme_.line);
  const auto text = Brush(dialogTarget.Get(), theme_.text);
  const auto muted = Brush(dialogTarget.Get(), theme_.muted);

  dialogTarget->BeginDraw();
  dialogTarget->Clear(Color(theme_.panel));
  dialogTarget->FillRectangle(D2D1::RectF(0, 0, size.width, footerTop), content.Get());
  dialogTarget->FillRectangle(D2D1::RectF(0, footerTop, size.width, size.height), panel.Get());
  dialogTarget->DrawLine(D2D1::Point2F(0, footerTop + 0.5f),
                         D2D1::Point2F(size.width, footerTop + 0.5f), line.Get());
  DrawText(dialogTarget.Get(), titleValue, D2D1::RectF(24.0f, 20.0f, size.width - 24.0f, 54.0f),
           titleFormat_.Get(), text.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
  DrawText(dialogTarget.Get(), messageValue,
           D2D1::RectF(28.0f, 67.0f, size.width - 28.0f, footerTop - 14.0f),
           bodyFormat_.Get(), muted.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, true);
  dialogTarget->EndDraw();
}

void NativeInstallerView::PaintDrivePopup(HWND hwnd,
                                          const std::vector<std::wstring>& drives,
                                          int selectedIndex,
                                          int hotIndex) {
  RECT client{};
  GetClientRect(hwnd, &client);
  ComPtr<ID2D1HwndRenderTarget> popupTarget;
  const auto properties = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN));
  const auto hwndProperties = D2D1::HwndRenderTargetProperties(
      hwnd, D2D1::SizeU(static_cast<UINT32>(std::max(1L, client.right)),
                        static_cast<UINT32>(std::max(1L, client.bottom))));
  if (FAILED(d2dFactory_->CreateHwndRenderTarget(properties, hwndProperties,
                                                  popupTarget.GetAddressOf()))) {
    return;
  }
  const float dpi = static_cast<float>(GetDpiForWindow(hwnd));
  popupTarget->SetDpi(dpi, dpi);
  popupTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

  const D2D1_SIZE_F size = popupTarget->GetSize();
  const auto input = Brush(popupTarget.Get(), theme_.input);
  const auto selected = Brush(popupTarget.Get(), Mix(theme_.input, theme_.accent, 0.10f));
  const auto hovered = Brush(popupTarget.Get(), theme_.panelAlt);
  const auto border = Brush(popupTarget.Get(), theme_.lineStrong);
  const auto text = Brush(popupTarget.Get(), theme_.text);
  const auto accent = Brush(popupTarget.Get(), theme_.accent);

  popupTarget->BeginDraw();
  popupTarget->Clear(Color(theme_.input));
  popupTarget->FillRectangle(D2D1::RectF(0, 0, size.width, size.height), input.Get());
  for (size_t i = 0; i < drives.size(); ++i) {
    const float top = 1.0f + static_cast<float>(i) * theme_.controlHeight;
    const D2D1_RECT_F row = D2D1::RectF(1.0f, top, size.width - 1.0f,
                                        top + theme_.controlHeight);
    if (static_cast<int>(i) == hotIndex) {
      popupTarget->FillRectangle(row, hovered.Get());
    } else if (static_cast<int>(i) == selectedIndex) {
      popupTarget->FillRectangle(row, selected.Get());
    }
    if (static_cast<int>(i) == selectedIndex) {
      const float centerY = (row.top + row.bottom) * 0.5f;
      popupTarget->DrawLine(D2D1::Point2F(10.0f, centerY),
                            D2D1::Point2F(14.0f, centerY + 4.0f), accent.Get(), 1.5f);
      popupTarget->DrawLine(D2D1::Point2F(14.0f, centerY + 4.0f),
                            D2D1::Point2F(21.0f, centerY - 4.0f), accent.Get(), 1.5f);
    }
    DrawText(popupTarget.Get(), drives[i],
             D2D1::RectF(30.0f, row.top, size.width - 8.0f, row.bottom),
             bodyFormat_.Get(), text.Get());
  }
  popupTarget->DrawRectangle(D2D1::RectF(0.5f, 0.5f, size.width - 0.5f,
                                          size.height - 0.5f), border.Get());
  popupTarget->EndDraw();
}

void NativeInstallerView::Resize(UINT width, UINT height) {
  if (target_) {
    target_->Resize(D2D1::SizeU(std::max(1U, width), std::max(1U, height)));
  }
}

void NativeInstallerView::DiscardDeviceResources() {
  target_.Reset();
  dcTarget_.Reset();
}

bool NativeInstallerView::DrawButton(const DRAWITEMSTRUCT& item,
                                     NativeButtonStyle style,
                                     bool hot) {
  if (FAILED(EnsureDcTarget(item.hwndItem)) ||
      FAILED(dcTarget_->BindDC(item.hDC, &item.rcItem))) {
    return false;
  }
  const bool disabled = (item.itemState & ODS_DISABLED) != 0;
  const bool pressed = (item.itemState & ODS_SELECTED) != 0;
  const bool focused = (item.itemState & ODS_FOCUS) != 0;
  const bool primary = style == NativeButtonStyle::Primary;
  const bool dangerStyle = style == NativeButtonStyle::Danger;
  const bool inputStyle = style == NativeButtonStyle::Input;

  uint32_t fill = inputStyle ? theme_.input : theme_.panelAlt;
  uint32_t border = theme_.lineStrong;
  uint32_t textColor = theme_.muted;
  if (disabled) {
    fill = Mix(theme_.panel, theme_.background, 0.45f);
    border = theme_.line;
    textColor = theme_.dim;
  } else if (pressed) {
    fill = theme_.input;
    border = primary ? theme_.accent : theme_.lineStrong;
    textColor = theme_.text;
  } else if (hot) {
    fill = Mix(theme_.panelAlt, theme_.text, 0.08f);
    border = dangerStyle ? theme_.danger : theme_.accent;
    textColor = theme_.accentStrong;
  } else if (primary) {
    fill = Mix(theme_.panelAlt, theme_.accent, 0.08f);
    border = theme_.accent;
    textColor = theme_.text;
  } else if (dangerStyle) {
    border = theme_.danger;
  }

  const auto fillBrush = Brush(dcTarget_.Get(), fill);
  const auto borderBrush = Brush(dcTarget_.Get(), border);
  const auto textBrush = Brush(dcTarget_.Get(), textColor);
  const D2D1_SIZE_F size = dcTarget_->GetSize();
  const D2D1_RECT_F rect = D2D1::RectF(0.5f, 0.5f, size.width - 0.5f, size.height - 0.5f);

  dcTarget_->BeginDraw();
  dcTarget_->Clear(Color(fill));
  dcTarget_->FillRoundedRectangle(
      D2D1::RoundedRect(rect, theme_.cornerRadius, theme_.cornerRadius), fillBrush.Get());
  dcTarget_->DrawRoundedRectangle(
      D2D1::RoundedRect(rect, theme_.cornerRadius, theme_.cornerRadius), borderBrush.Get());

  wchar_t caption[128]{};
  GetWindowTextW(item.hwndItem, caption, static_cast<int>(std::size(caption)));
  D2D1_RECT_F textRect = rect;
  if (pressed) {
    textRect.top += 1.0f;
    textRect.bottom += 1.0f;
  }
  if (inputStyle) {
    textRect.left += 9.0f;
    textRect.right -= 30.0f;
  }
  DrawText(dcTarget_.Get(), caption, textRect,
           inputStyle ? bodyFormat_.Get() : buttonFormat_.Get(), textBrush.Get(),
           inputStyle ? DWRITE_TEXT_ALIGNMENT_LEADING : DWRITE_TEXT_ALIGNMENT_CENTER);
  if (inputStyle) {
    const float centerY = (rect.top + rect.bottom) * 0.5f;
    dcTarget_->DrawLine(D2D1::Point2F(rect.right - 20.0f, centerY - 2.5f),
                        D2D1::Point2F(rect.right - 15.0f, centerY + 2.5f),
                        textBrush.Get(), 1.4f);
    dcTarget_->DrawLine(D2D1::Point2F(rect.right - 15.0f, centerY + 2.5f),
                        D2D1::Point2F(rect.right - 10.0f, centerY - 2.5f),
                        textBrush.Get(), 1.4f);
  }
  if (focused && !disabled) {
    const D2D1_RECT_F focus = Inset(rect, 4.0f);
    dcTarget_->DrawRoundedRectangle(D2D1::RoundedRect(focus, 1.0f, 1.0f),
                                    borderBrush.Get(), 1.0f);
  }
  return SUCCEEDED(dcTarget_->EndDraw());
}

bool NativeInstallerView::DrawComboItem(const DRAWITEMSTRUCT& item) {
  if (FAILED(EnsureDcTarget(item.hwndItem)) ||
      FAILED(dcTarget_->BindDC(item.hDC, &item.rcItem))) {
    return false;
  }
  const bool selected = (item.itemState & ODS_SELECTED) != 0;
  const bool disabled = (item.itemState & ODS_DISABLED) != 0;
  const uint32_t fill = selected ? Mix(theme_.panelAlt, theme_.accent, 0.16f) : theme_.input;
  const uint32_t textColor = disabled ? theme_.dim : theme_.text;
  const auto textBrush = Brush(dcTarget_.Get(), textColor);
  const D2D1_SIZE_F size = dcTarget_->GetSize();

  dcTarget_->BeginDraw();
  dcTarget_->Clear(Color(fill));
  UINT itemId = item.itemID;
  if (itemId == static_cast<UINT>(-1)) {
    itemId = static_cast<UINT>(SendMessageW(item.hwndItem, CB_GETCURSEL, 0, 0));
  }
  if (itemId != static_cast<UINT>(CB_ERR) && itemId != static_cast<UINT>(-1)) {
    const int length = static_cast<int>(SendMessageW(item.hwndItem, CB_GETLBTEXTLEN, itemId, 0));
    if (length > 0) {
      std::wstring value(static_cast<size_t>(length) + 1, L'\0');
      SendMessageW(item.hwndItem, CB_GETLBTEXT, itemId,
                   reinterpret_cast<LPARAM>(value.data()));
      value.resize(static_cast<size_t>(length));
      DrawText(dcTarget_.Get(), value, D2D1::RectF(9.0f, 0, size.width - 4.0f, size.height),
               bodyFormat_.Get(), textBrush.Get());
    }
  }
  return SUCCEEDED(dcTarget_->EndDraw());
}

COLORREF NativeInstallerView::TextColor() const {
  return WinColor(theme_.text);
}

COLORREF NativeInstallerView::MutedColor() const {
  return WinColor(theme_.muted);
}

COLORREF NativeInstallerView::InputColor() const {
  return WinColor(theme_.input);
}

}  // namespace modlist
