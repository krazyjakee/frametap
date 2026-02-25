#include <frametap/frametap.h>

#include "backend.h"

namespace frametap {

// --- Free functions --------------------------------------------------------

std::vector<Monitor> get_monitors() { return internal::enumerate_monitors(); }
std::vector<Window> get_windows() { return internal::enumerate_windows(); }
PermissionCheck check_permissions() {
  return internal::check_platform_permissions();
}

// --- FrameTap::Impl --------------------------------------------------------

struct FrameTap::Impl {
  std::unique_ptr<internal::Backend> backend;
  FrameCallback callback;
  bool started = false;
};

// --- Constructors / move / destructor --------------------------------------

FrameTap::FrameTap() : impl_(std::make_unique<Impl>()) {
  impl_->backend = internal::make_backend();
}

FrameTap::FrameTap(Rect region) : impl_(std::make_unique<Impl>()) {
  impl_->backend = internal::make_backend(region);
}

FrameTap::FrameTap(Monitor monitor) : impl_(std::make_unique<Impl>()) {
  impl_->backend = internal::make_backend(monitor);
}

FrameTap::FrameTap(Window window) : impl_(std::make_unique<Impl>()) {
  impl_->backend = internal::make_backend(window);
}

FrameTap::~FrameTap() = default;
FrameTap::FrameTap(FrameTap &&) noexcept = default;
FrameTap &FrameTap::operator=(FrameTap &&) noexcept = default;

// --- Configuration ---------------------------------------------------------

void FrameTap::set_region(Rect region) {
  impl_->backend->set_region(region);
}

void FrameTap::on_frame(FrameCallback callback) {
  impl_->callback = std::move(callback);
}

// --- Capture control -------------------------------------------------------

void FrameTap::start() {
  if (!impl_->callback)
    throw CaptureError("No frame callback set");
  impl_->backend->start(impl_->callback);
  impl_->started = true;
}

void FrameTap::start_async() {
  if (!impl_->callback)
    throw CaptureError("No frame callback set");
  impl_->backend->start(impl_->callback);
  impl_->started = true;
}

void FrameTap::stop() {
  if (impl_->started) {
    impl_->backend->stop();
    impl_->started = false;
  }
}

void FrameTap::pause() { impl_->backend->pause(); }
void FrameTap::resume() { impl_->backend->resume(); }
bool FrameTap::is_paused() const { return impl_->backend->is_paused(); }

// --- Screenshot ------------------------------------------------------------

ImageData FrameTap::screenshot() { return impl_->backend->screenshot({}); }

ImageData FrameTap::screenshot(Rect region) {
  return impl_->backend->screenshot(region);
}

} // namespace frametap
