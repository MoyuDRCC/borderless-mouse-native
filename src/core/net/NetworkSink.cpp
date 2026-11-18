#include "core/net/NetworkSink.h"

#include <cmath>
#include <stdexcept>

namespace bm {

NetworkSink::NetworkSink(PeerSessionPtr session) : session_(std::move(session)) {
  if (!session_) throw std::invalid_argument("网络出口必须绑定会话");
}

const DesktopGeometry& NetworkSink::RemoteGeometry() const { return session_->PeerGeometry(); }

bool NetworkSink::IsClosed() const { return session_->IsClosed(); }

bool NetworkSink::IsActive() const {
  std::lock_guard lock(gate_);
  return activeControlId_ != 0 && !session_->IsClosed();
}

uint64_t NetworkSink::ControlId() const {
  std::lock_guard lock(gate_);
  return lastControlId_;
}

double NetworkSink::PointerScale() const { return pointerScale_.load(std::memory_order_relaxed); }

void NetworkSink::BeginControl() {
  std::lock_guard lock(gate_);
  if (activeControlId_ != 0) throw std::logic_error("进入新目标前必须结束已有控制");
  entryReady_ = true;
}

bool NetworkSink::MoveTo(int x, int y) {
  std::lock_guard lock(gate_);
  if (session_->IsClosed()) return false;
  if (activeControlId_ == 0) {
    if (!entryReady_) return false;
    // 代次只在显式进入时分配，Yield 之后普通移动不能重新取得控制
    if (lastControlId_ == UINT64_MAX) throw std::overflow_error("控制代次已耗尽");
    const uint64_t controlId = ++lastControlId_;
    entryReady_ = false;
    const Bytes start = ControlPayload(controlId);
    if (!session_->TrySend(MessageType::ControlStart, start)) return false;
    activeControlId_ = controlId;
  }
  // 远端几何是唯一可见区域来源，越界位置在这里夹取
  const Point2i visible = session_->PeerGeometry().ClampPixel(Point2i{x, y});
  return session_->TrySendPosition(InputCodec::Position(visible.x, visible.y, activeControlId_));
}

bool NetworkSink::Button(MouseButton button, bool down) {
  std::lock_guard lock(gate_);
  if (activeControlId_ == 0 || session_->IsClosed()) return false;
  return session_->TrySend(MessageType::Input,
                           InputCodec::Button(static_cast<int>(button), down, activeControlId_));
}

bool NetworkSink::Wheel(int delta, bool horizontal) {
  std::lock_guard lock(gate_);
  if (activeControlId_ == 0 || session_->IsClosed()) return false;
  const int value = ScaleWheel(delta, horizontal);
  // 缩放后为 0 也要交给会话吞掉，否则会退回本机自己滚动
  if (value == 0) return true;
  return session_->TrySend(MessageType::Input, InputCodec::Wheel(value, horizontal, activeControlId_));
}

int NetworkSink::ScaleWheel(int delta, bool horizontal) const {
  int scaled = static_cast<int>(std::lround(delta * wheelScalePercent_ / 100.0));
  if (scaled == 0 && delta != 0) scaled = delta > 0 ? 1 : -1;
  if (!horizontal && wheelReverse_) scaled = -scaled;
  return scaled;
}

bool NetworkSink::Key(int virtualKey, int scanCode, bool extended, bool down) {
  std::lock_guard lock(gate_);
  if (activeControlId_ == 0 || session_->IsClosed()) return false;
  return session_->TrySend(MessageType::Input,
                           InputCodec::Key(virtualKey, scanCode, extended, down, activeControlId_));
}

void NetworkSink::ReleaseControl() {
  std::lock_guard lock(gate_);
  const uint64_t controlId = activeControlId_;
  activeControlId_ = 0;
  entryReady_ = false;
  if (controlId != 0 && !session_->IsClosed()) session_->Send(MessageType::ControlEnd, ControlPayload(controlId));
}

bool NetworkSink::HandleYield(uint64_t controlId) {
  std::lock_guard lock(gate_);
  if (controlId == 0 || controlId != activeControlId_) return false;
  activeControlId_ = 0;
  entryReady_ = false;
  return true;
}

}  // namespace bm
