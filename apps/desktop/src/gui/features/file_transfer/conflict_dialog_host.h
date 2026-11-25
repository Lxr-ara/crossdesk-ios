#ifndef CROSSDESK_GUI_CONFLICT_DIALOG_HOST_H_
#define CROSSDESK_GUI_CONFLICT_DIALOG_HOST_H_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "file_transfer.h"

namespace crossdesk {

// 收端覆盖冲突的提问端。
//
// 数据面在接收线程上撞到「目标已存在」，按 kAskSkip 就必须问人；问题本身由
// FileTransferManager::Poll()（UI 定时器）显示成一个自带四个按钮的小窗口：
//   * Ask()   接收线程：排队 + 阻塞，等答案返回；
//   * Pump()  UI 线程：把待回答的问题画到窗口上 / 收起窗口；
//   * Resolve() UI 线程：按钮回调，把答案交给等待中的接收线程。
// Slint 只在 UI 线程被碰，所以这里的互斥量只保护普通数据。
//
// 「全部应用」是批次级的：按对端记账，回答之后同一批里剩下的文件不再问第二
// 次；批次结束时管理器调用 EndBatch() 把它清掉，下一次批量重新问。
class ConflictDialogHost {
 public:
  using Choice = ConflictChoice;
  // 没人回答（无人值守、窗口被系统挂起）时的上限。到点按「跳过」收尾，绝不让
  // 一条接收线程永远挂着。
  static constexpr auto kAnswerTimeout = std::chrono::minutes(10);

  ConflictDialogHost();
  ~ConflictDialogHost();
  ConflictDialogHost(const ConflictDialogHost&) = delete;
  ConflictDialogHost& operator=(const ConflictDialogHost&) = delete;

  // 安装给 FileReceiver::SetConflictResolver 的钩子本体。destination 是本次
  // 传输请求的完整目标路径，size 是刚刚发布到磁盘的那个文件的大小。
  Choice Ask(const std::string& remote_id, const std::string& destination,
             uint64_t size);
  // UI 线程，每个 UI tick 调一次：显示/更新/收起窗口。language_index 是
  // localization 目录里的语言下标，用作按钮文案。
  void Pump(int language_index);
  // 这一批接收结束了：忘掉它的「全部应用」，下一次重新问。
  void EndBatch(const std::string& remote_id);
  std::vector<std::string> BatchRemotes() const;
  // UI 线程：按钮（或 Esc）的回答落到这里。
  void Resolve(Choice choice);
  // 关窗/退出：唤醒仍然在等答案的接收线程，之后每一次 Ask() 直接回「跳过」。
  void Shutdown();
  // 正在显示的问题编号，0 表示没有。给管理器的日志和测试用。
  uint64_t PendingRequest() const;

 private:
  void Show(const std::string& destination, uint64_t size, uint64_t request_id,
            int language_index);
  void Hide();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace crossdesk

#endif
