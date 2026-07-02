#pragma once

#include <windows.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

struct ICoreWebView2;
struct ICoreWebView2Controller;

namespace modlist {

class WebViewHost {
public:
  using MessageHandler = std::function<void(const std::wstring&)>;
  using ReadyHandler = std::function<void()>;
  using ErrorHandler = std::function<void(HRESULT)>;

  WebViewHost();
  ~WebViewHost();

  WebViewHost(const WebViewHost&) = delete;
  WebViewHost& operator=(const WebViewHost&) = delete;

  void Initialize(HWND parent,
                  const std::filesystem::path& htmlPath,
                  MessageHandler messageHandler,
                  ReadyHandler readyHandler,
                  ErrorHandler errorHandler);
  void Resize();
  void PostJson(const std::wstring& json);

  bool IsReady() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace modlist
