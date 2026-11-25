//
// Bridges the desktop transfer manager to the controller's TransferControl
// interface, so a phone client can list, pause, resume, retry and tune transfers
// over the same file_browser RPC the desktop window uses internally.
//
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <file_browser.h>

#include "config_center/config_center.h"
#include "gui/features/file_transfer/file_transfer_manager.h"

namespace crossdesk {

class GuiTransferControl final : public TransferControl {
 public:
  GuiTransferControl(FileTransferManager& transfers, ConfigCenter& config)
      : transfers_(transfers), config_(config) {}

  void ListTransfers(const std::string& remote,
                     std::vector<TransferRowView>* rows,
                     TransferSummaryView* summary) override {
    if (rows) rows->clear();
    for (const auto& row : transfers_.TransferRows(remote)) {
      if (!rows) break;
      TransferRowView view;
      view.id = row.file_id;
      view.name = row.file_name;
      view.path = Utf8Of(row.file_path);
      view.size = row.file_size;
      view.sent = row.sent_bytes;
      view.rate = row.rate_bps;
      view.state = StateName(row.state);
      view.paused = row.paused;
      view.retryable = row.retryable;
      view.receiving = row.receiving;
      rows->push_back(std::move(view));
    }
    if (summary) {
      const auto board = transfers_.BatchSummary(remote);
      summary->total = board.total;
      summary->completed = board.completed;
      summary->failed = board.failed;
      summary->active = board.active;
      summary->queued = board.queued;
      summary->paused = board.paused;
      summary->batch_partial = board.batch_partial;
    }
  }

  bool Pause(const std::string& remote, uint32_t id) override {
    return transfers_.PauseTransferItem(remote, id);
  }
  bool Resume(const std::string& remote, uint32_t id) override {
    return transfers_.ResumeTransferItem(remote, id);
  }
  bool Retry(const std::string& remote, uint32_t id) override {
    return transfers_.RetryTransferItem(remote, id);
  }
  bool Cancel(const std::string& remote, uint32_t id) override {
    // CancelBrowserTransfer returns void, so answer from the queue itself: the
    // caller is told applied only when the row existed and was still running.
    bool live = false;
    for (const auto& row : transfers_.TransferRows(remote)) {
      if (row.file_id != id) continue;
      live = row.receiving && !row.paused &&
        row.state != file_transfer::SendQueue::State::Completed &&
        row.state != file_transfer::SendQueue::State::Failed &&
        row.state != file_transfer::SendQueue::State::Cancelled;
      break;
    }
    if (!live) return false;
    transfers_.CancelBrowserTransfer(remote, id);
    return true;
  }
  bool StartAll(const std::string& remote) override {
    return transfers_.StartAllTransfers(remote);
  }
  bool PauseAll(const std::string& remote) override {
    return transfers_.PauseAllTransfers(remote);
  }
  bool ResumeAll(const std::string& remote) override {
    return transfers_.ResumeAllTransfers(remote);
  }
  bool ClearAll(const std::string& remote) override {
    return transfers_.ClearAllTransfers(remote);
  }
  bool RemoveFinished(const std::string& remote) override {
    return transfers_.RemoveFinishedTransfers(remote);
  }
  void SetRateKbps(int kbps) override { config_.SetTransferRateLimitKbps(kbps); }
  int RateKbps() override { return config_.GetTransferRateLimitKbps(); }
  void SetOverwritePolicy(int policy) override {
    config_.SetTransferOverwritePolicy(policy);
  }
  int OverwritePolicy() override { return config_.GetTransferOverwritePolicy(); }
  void SetConcurrency(int limit) override {
    transfers_.SetConcurrentTransfersPerPeer(static_cast<size_t>(limit));
  }
  int Concurrency() override {
    return static_cast<int>(transfers_.ConcurrentTransfersPerPeer());
  }

 private:
  // The controller keeps its own state vocabulary; the row view carries the name
  // so a phone client never has to learn the enum.
  static std::string StateName(file_transfer::SendQueue::State state) {
    switch (state) {
      case file_transfer::SendQueue::State::Queued: return "queued";
      case file_transfer::SendQueue::State::Sending: return "sending";
      case file_transfer::SendQueue::State::Paused: return "paused";
      case file_transfer::SendQueue::State::Completed: return "completed";
      case file_transfer::SendQueue::State::Failed: return "failed";
      case file_transfer::SendQueue::State::Cancelled: return "cancelled";
    }
    return "queued";
  }
  // TransferRow carries a UTF-8 std::string already; the path comes from the
  // filesystem, so convert once here instead of letting the JSON layer guess.
  static std::string Utf8Of(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
  }

  FileTransferManager& transfers_;
  ConfigCenter& config_;
};

}  // namespace crossdesk
