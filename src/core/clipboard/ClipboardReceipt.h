#pragma once

#include <cstdint>
#include <functional>
#include <mutex>

namespace bm {

// 撤销信号：批次被放弃时用来跳出未完成的发送或落盘工作，与协议/IO 错误区分开
struct TransferCancelled {};

// 一次接收的提交权。取消与最终剪贴板写入在同一把锁内线性化，不调用传输或剪贴板状态锁
class ClipboardReceipt {
 public:
  ClipboardReceipt() = default;
  ClipboardReceipt(const ClipboardReceipt&) = delete;
  ClipboardReceipt& operator=(const ClipboardReceipt&) = delete;

  // 接收代次；由剪贴板状态在授予接收权时填写
  int64_t generation = 0;

  bool IsValid() {
    std::lock_guard<std::mutex> lock(mu_);
    return !cancelled_;
  }

  // 撤销提交权并通知持有者一次；可以从界面、会话或清理线程调用
  void Cancel() {
    std::function<void()> handler;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (cancelled_) return;
      cancelled_ = true;
      handler = std::move(onCancel_);
      onCancel_ = nullptr;
    }
    // 回调会进入传输对象，必须在释放本锁之后再触发，避免与写入路径互相等待
    if (handler) handler();
  }

  // 取消与最终写入线性化：已被撤销则返回 false 且不执行写入
  bool TryCommit(const std::function<void()>& write, const std::function<void()>& applied) {
    std::lock_guard<std::mutex> lock(mu_);
    if (cancelled_) return false;
    write();
    if (applied) applied();
    return true;
  }

  // 撤销时的通知：只做标记与入队，不得等待传输锁；已撤销时立即通知
  void SetCancelHandler(std::function<void()> handler) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!cancelled_) {
        onCancel_ = std::move(handler);
        return;
      }
    }
    if (handler) handler();
  }

 private:
  std::mutex mu_;
  bool cancelled_ = false;
  std::function<void()> onCancel_;
};

}  // namespace bm
