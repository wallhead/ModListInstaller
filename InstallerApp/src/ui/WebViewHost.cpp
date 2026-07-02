#define NOMINMAX

#include "ui/WebViewHost.h"

#include <WebView2.h>
#include <shlwapi.h>
#include <wrl.h>

#include <utility>

namespace modlist {

namespace {

std::wstring LocalAppDataFolder() {
  wchar_t buffer[MAX_PATH]{};
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    wchar_t temp[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temp) > 0) {
      return temp;
    }
    return L".";
  }
  return buffer;
}

std::wstring FileUrlFromPath(const std::filesystem::path& path) {
  wchar_t url[4096]{};
  DWORD urlLength = static_cast<DWORD>(std::size(url));
  if (SUCCEEDED(UrlCreateFromPathW(path.c_str(), url, &urlLength, 0))) {
    return std::wstring(url, urlLength);
  }
  return L"file:///" + path.wstring();
}

}  // namespace

struct WebViewHost::Impl {
  HWND parent = nullptr;
  Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller;
  Microsoft::WRL::ComPtr<ICoreWebView2> webView;
  MessageHandler messageHandler;
  ReadyHandler readyHandler;
  ErrorHandler errorHandler;
  bool ready = false;

  void Resize() {
    if (controller == nullptr || parent == nullptr) {
      return;
    }
    RECT bounds{};
    GetClientRect(parent, &bounds);
    controller->put_Bounds(bounds);
  }
};

WebViewHost::WebViewHost() : impl_(std::make_shared<Impl>()) {}

WebViewHost::~WebViewHost() = default;

void WebViewHost::Initialize(HWND parent,
                             const std::filesystem::path& htmlPath,
                             MessageHandler messageHandler,
                             ReadyHandler readyHandler,
                             ErrorHandler errorHandler) {
  impl_->parent = parent;
  impl_->messageHandler = std::move(messageHandler);
  impl_->readyHandler = std::move(readyHandler);
  impl_->errorHandler = std::move(errorHandler);

  const std::filesystem::path userDataFolder =
      std::filesystem::path(LocalAppDataFolder()) / "ModlistInstaller" / "WebView2";
  const std::wstring userData = userDataFolder.wstring();
  const std::wstring targetUrl = FileUrlFromPath(htmlPath);
  std::weak_ptr<Impl> weakImpl = impl_;

  HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
      nullptr,
      userData.c_str(),
      nullptr,
      Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [weakImpl, targetUrl](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
            auto impl = weakImpl.lock();
            if (impl == nullptr) {
              return S_OK;
            }
            if (FAILED(result) || environment == nullptr) {
              if (impl->errorHandler) {
                impl->errorHandler(result);
              }
              return S_OK;
            }

            environment->CreateCoreWebView2Controller(
                impl->parent,
                Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [weakImpl, targetUrl](HRESULT controllerResult, ICoreWebView2Controller* controller) -> HRESULT {
                      auto impl = weakImpl.lock();
                      if (impl == nullptr) {
                        return S_OK;
                      }
                      if (FAILED(controllerResult) || controller == nullptr) {
                        if (impl->errorHandler) {
                          impl->errorHandler(controllerResult);
                        }
                        return S_OK;
                      }

                      impl->controller = controller;
                      impl->controller->get_CoreWebView2(&impl->webView);
                      impl->Resize();

                      Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
                      if (SUCCEEDED(impl->webView->get_Settings(&settings)) && settings != nullptr) {
                        settings->put_AreDefaultContextMenusEnabled(FALSE);
                        settings->put_AreDevToolsEnabled(FALSE);
                      }

                      EventRegistrationToken messageToken{};
                      impl->webView->add_WebMessageReceived(
                          Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                              [weakImpl](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                auto impl = weakImpl.lock();
                                if (impl == nullptr) {
                                  return S_OK;
                                }
                                LPWSTR raw = nullptr;
                                if (SUCCEEDED(args->get_WebMessageAsJson(&raw)) && raw != nullptr) {
                                  std::wstring message(raw);
                                  CoTaskMemFree(raw);
                                  if (impl->messageHandler) {
                                    impl->messageHandler(message);
                                  }
                                }
                                return S_OK;
                              })
                              .Get(),
                          &messageToken);

                      EventRegistrationToken navigationToken{};
                      impl->webView->add_NavigationCompleted(
                          Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
                              [weakImpl](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                auto impl = weakImpl.lock();
                                if (impl == nullptr) {
                                  return S_OK;
                                }
                                BOOL success = FALSE;
                                args->get_IsSuccess(&success);
                                if (success) {
                                  impl->ready = true;
                                  if (impl->readyHandler) {
                                    impl->readyHandler();
                                  }
                                }
                                return S_OK;
                              })
                              .Get(),
                          &navigationToken);

                      const HRESULT navigateResult = impl->webView->Navigate(targetUrl.c_str());
                      if (FAILED(navigateResult) && impl->errorHandler) {
                        impl->errorHandler(navigateResult);
                      }
                      return S_OK;
                    })
                    .Get());
            return S_OK;
          })
          .Get());

  if (FAILED(hr) && impl_->errorHandler) {
    impl_->errorHandler(hr);
  }
}

void WebViewHost::Resize() {
  impl_->Resize();
}

void WebViewHost::PostJson(const std::wstring& json) {
  if (impl_->webView != nullptr && impl_->ready) {
    impl_->webView->PostWebMessageAsJson(json.c_str());
  }
}

bool WebViewHost::IsReady() const {
  return impl_->ready;
}

}  // namespace modlist
