// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "WindowsWgcSource.h"
#include <QMutex>
#include <QMutexLocker>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
using namespace winrt;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
namespace deskflow::streaming {
struct WindowsWgcSource::State {
  QMutex mutex;
  GraphicsCaptureItem item{nullptr};
  Direct3D11CaptureFramePool pool{nullptr};
  GraphicsCaptureSession session{nullptr};
  IDirect3DDevice device{nullptr};
  com_ptr<ID3D11Device> d3d;
  com_ptr<ID3D11DeviceContext> context;
  com_ptr<ID3D11Texture2D> staging;
  SizeInt32 size{};
  event_token arrived{}, closed{};
  QString error;
  std::optional<VideoFrame> latest;
  bool running = false;
  qint64 previousNs = 0, intervalNs = 33333333;
};
WindowsWgcSource::WindowsWgcSource() = default;
WindowsWgcSource::~WindowsWgcSource() { stop(); }
void WindowsWgcSource::stop()
{
  auto state = std::exchange(m_state, {});
  if (!state) return;
  { QMutexLocker lock(&state->mutex); state->running = false; state->latest.reset(); }
  try {
    if (state->pool) state->pool.FrameArrived(state->arrived);
    if (state->item) state->item.Closed(state->closed);
    if (state->session) state->session.Close();
    if (state->pool) state->pool.Close();
  } catch (const hresult_error &) { /* The OS may have already closed these resources. */ }
}
bool WindowsWgcSource::start(quint64 window, quint64 monitor, int fps, QString &error)
{
  stop();
  if ((window == 0) == (monitor == 0)) { error = "Exactly one nonzero window or monitor handle is required"; return false; }
  auto state = std::make_shared<State>();
  m_state = state;
  try {
    auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    if (window)
      check_hresult(interop->CreateForWindow(reinterpret_cast<HWND>(window), guid_of<GraphicsCaptureItem>(), put_abi(state->item)));
    else
      check_hresult(interop->CreateForMonitor(reinterpret_cast<HMONITOR>(monitor), guid_of<GraphicsCaptureItem>(), put_abi(state->item)));
    const auto selectedMonitor = window ? MonitorFromWindow(reinterpret_cast<HWND>(window), MONITOR_DEFAULTTONULL)
                                        : reinterpret_cast<HMONITOR>(monitor);
    com_ptr<IDXGIFactory1> factory;
    check_hresult(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void()));
    com_ptr<IDXGIAdapter1> selectedAdapter;
    for (UINT i = 0; !selectedAdapter; ++i) {
      com_ptr<IDXGIAdapter1> adapter;
      const auto result = factory->EnumAdapters1(i, adapter.put());
      if (result == DXGI_ERROR_NOT_FOUND) break;
      check_hresult(result);
      for (UINT j = 0; ; ++j) {
        com_ptr<IDXGIOutput> output;
        const auto next = adapter->EnumOutputs(j, output.put());
        if (next == DXGI_ERROR_NOT_FOUND) break;
        check_hresult(next);
        DXGI_OUTPUT_DESC description{}; check_hresult(output->GetDesc(&description));
        if (description.Monitor == selectedMonitor) { selectedAdapter = adapter; break; }
      }
    }
    if (!selectedAdapter) { error = "Could not resolve the selected display's graphics adapter"; stop(); return false; }
    check_hresult(D3D11CreateDevice(selectedAdapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, state->d3d.put(), nullptr, state->context.put()));
    auto dxgi = state->d3d.as<IDXGIDevice>();
    com_ptr<IInspectable> inspectable;
    check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
    state->device = inspectable.as<IDirect3DDevice>();
    state->size = state->item.Size();
    state->intervalNs = 1000000000LL / fps;
    state->pool = Direct3D11CaptureFramePool::CreateFreeThreaded(state->device,
      DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, state->size);
    state->closed = state->item.Closed([state](auto &&, auto &&) {
      QMutexLocker lock(&state->mutex);
      state->running = false; state->latest.reset(); state->error = "Windows closed the selected capture item";
    });
    state->arrived = state->pool.FrameArrived([state](auto &&pool, auto &&) {
      QMutexLocker lock(&state->mutex);
      if (!state->running) return;
      try {
        auto captured = pool.TryGetNextFrame();
        if (!captured) return;
        const auto timeNs = captured.SystemRelativeTime().count() * 100;
        if (state->previousNs && timeNs - state->previousNs < state->intervalNs) return;
        const auto size = captured.ContentSize();
        if (size.Width <= 0 || size.Height <= 0) return;
        if (size.Width != state->size.Width || size.Height != state->size.Height) {
          captured.Close();
          state->size = size; state->staging = nullptr;
          pool.Recreate(state->device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
          return; // the next frame belongs to the new geometry
        }
        auto surface = captured.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        com_ptr<ID3D11Texture2D> texture;
        check_hresult(surface->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
        D3D11_TEXTURE2D_DESC description{}; texture->GetDesc(&description);
        if (!state->staging) {
          description.Usage = D3D11_USAGE_STAGING; description.BindFlags = 0;
          description.CPUAccessFlags = D3D11_CPU_ACCESS_READ; description.MiscFlags = 0;
          check_hresult(state->d3d->CreateTexture2D(&description, nullptr, state->staging.put()));
        }
        state->context->CopyResource(state->staging.get(), texture.get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check_hresult(state->context->Map(state->staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
        VideoFrame frame;
        frame.pixels = QImage(static_cast<const uchar *>(mapped.pData), size.Width, size.Height,
                             mapped.RowPitch, QImage::Format_ARGB32).copy();
        state->context->Unmap(state->staging.get(), 0);
        frame.captureTimeNs = timeNs; frame.mediaTimeNs = timeNs;
        state->previousNs = timeNs; state->latest = std::move(frame);
      } catch (const hresult_error &failure) {
        state->running = false; state->latest.reset(); state->error = QString::fromWCharArray(failure.message().c_str());
      }
    });
    state->session = state->pool.CreateCaptureSession(state->item);
    state->session.IsCursorCaptureEnabled(true);
    state->session.IsBorderRequired(true);
    { QMutexLocker lock(&state->mutex); state->running = true; }
    state->session.StartCapture();
    return true;
  } catch (const hresult_error &failure) {
    error = QString::fromWCharArray(failure.message().c_str()); stop(); return false;
  }
}
std::optional<VideoFrame> WindowsWgcSource::pull(QString &error)
{
  if (!m_state) return {};
  QMutexLocker lock(&m_state->mutex);
  error = m_state->error;
  return std::exchange(m_state->latest, {});
}
}
