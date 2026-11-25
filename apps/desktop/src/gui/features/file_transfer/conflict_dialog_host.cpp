#include "features/file_transfer/conflict_dialog_host.h"

#include <algorithm>
#include <cstdio>
#include <utility>

#include "crossdesk_ui.h"
#include "localization.h"
#include "rd_log.h"

namespace crossdesk {

namespace {

constexpr size_t kMaxBatches = 64;
constexpr size_t kMaxDestinationBytes = 1024;

// 只给人看，不做精确统计：三档加字节就够。
std::string FormatSize(uint64_t bytes) {
  constexpr double kKib = 1024.0;
  const double value = static_cast<double>(bytes);
  char text[64] = {};
  if (bytes >= 1024ull * 1024 * 1024) {
    std::snprintf(text, sizeof(text), "%.2f GB", value / (kKib * kKib * kKib));
  } else if (bytes >= 1024ull * 1024) {
    std::snprintf(text, sizeof(text), "%.2f MB", value / (kKib * kKib));
  } else if (bytes >= 1024) {
    std::snprintf(text, sizeof(text), "%.1f KB", value / kKib);
  } else {
    std::snprintf(text, sizeof(text), "%llu B",
                  static_cast<unsigned long long>(bytes));
  }
  return text;
}

size_t LastSeparator(const std::string& path) {
  const auto slash = path.find_last_of("/\\");
  return slash == std::string::npos ? std::string::npos : slash;
}

std::string FileNameOf(const std::string& path) {
  const auto slash = LastSeparator(path);
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// 窗口里显示的「目标文件夹」：最后一个分隔符之前的部分。没有分隔符就退化成
// 当前目录，绝不会把整条路径当成目录再拼一次。
std::string FolderOf(const std::string& path) {
  const auto slash = LastSeparator(path);
  if (slash == std::string::npos) return ".";
  if (slash == 0) return path.substr(0, 1);
  return path.substr(0, slash);
}

bool IsBatchChoice(ConflictChoice choice) {
  return choice == ConflictChoice::kSkipAll || choice == ConflictChoice::kOverwriteAll ||
         choice == ConflictChoice::kRenameAll;
}

std::optional<ConflictChoice> BaseChoice(int index) {
  switch (index) {
    case 0: return ConflictChoice::kSkip;
    case 1: return ConflictChoice::kOverwrite;
    case 2: return ConflictChoice::kRename;
    default: return std::nullopt;
  }
}

ConflictChoice AllVariant(ConflictChoice base) {
  switch (base) {
    case ConflictChoice::kOverwrite:
    case ConflictChoice::kOverwriteAll: return ConflictChoice::kOverwriteAll;
    case ConflictChoice::kRename:
    case ConflictChoice::kRenameAll: return ConflictChoice::kRenameAll;
    default: return ConflictChoice::kSkipAll;
  }
}

}  // namespace

struct ConflictDialogHost::Impl {
  struct Question {
    uint64_t id = 0;
    std::string remote_id;
    std::string destination;
    uint64_t size = 0;
  };

  mutable std::mutex mutex;
  std::condition_variable answered;
  // 同一时刻只有一个问题在等回答；后来的 Ask() 在前一个回答之后才排队。
  std::optional<Question> pending;
  uint64_t answer_id = 0;
  ConflictChoice answer = ConflictChoice::kSkip;
  uint64_t next_request = 0;
  bool stopped = false;
  // 批次（「全部应用」）：只在本次批量存活期内有效。
  std::map<std::string, ConflictChoice> batches;
  // 窗口状态；只在 UI 线程读写（Pump/Hide），用 mutex 只是为了给
  // PendingRequest() 与 Resolve() 一个一致视图。
  std::optional<slint::ComponentHandle<ui::ConflictDialog>> window;
  uint64_t shown_id = 0;
  int shown_language = -1;
  bool showing = false;
};

ConflictDialogHost::ConflictDialogHost() : impl_(std::make_unique<Impl>()) {}

ConflictDialogHost::~ConflictDialogHost() { Shutdown(); }

ConflictChoice ConflictDialogHost::Ask(const std::string& remote_id,
                                      const std::string& destination, uint64_t size) {
  std::unique_lock lock(impl_->mutex);
  if (impl_->stopped) return ConflictChoice::kSkip;
  // 这一批已经选过「全部」：同一个答案直接复用，绝不第二次弹窗。
  if (const auto batch = impl_->batches.find(remote_id); batch != impl_->batches.end())
    return batch->second;
  // 一个窗口一次只问一件事：后面的问题等前一个回答完再问，而不是并排弹两个
  // 一模一样的对话框。
  impl_->answered.wait(lock, [this] { return impl_->stopped || !impl_->pending; });
  if (impl_->stopped) return ConflictChoice::kSkip;
  if (const auto batch = impl_->batches.find(remote_id); batch != impl_->batches.end())
    return batch->second;

  Impl::Question question;
  question.id = ++impl_->next_request;
  question.remote_id = remote_id;
  question.destination =
      destination.size() > kMaxDestinationBytes ? destination.substr(0, kMaxDestinationBytes)
                                                : destination;
  question.size = size;
  impl_->pending = question;
  const auto id = question.id;
  const auto mine = [this, id] { return impl_->stopped || impl_->answer_id == id; };
  const auto deadline = std::chrono::steady_clock::now() + kAnswerTimeout;
  if (!impl_->answered.wait_until(lock, deadline, mine)) {
    // 没人回答：撤掉问题，按「跳过」收尾，后一个等待者立刻能拿到窗口。
    if (impl_->pending && impl_->pending->id == id) impl_->pending.reset();
    impl_->answer_id = id;
    // 没有这一句，第二个停在同一把条件变量上的接收线程会永远等下去。
    impl_->answered.notify_all();  // 超时：把窗口让给下一个提问者
    LOG_WARN("[transfer] conflict prompt timed out for {}", question.destination);
    return ConflictChoice::kSkip;
  }
  if (impl_->stopped) {
    if (impl_->pending && impl_->pending->id == id) impl_->pending.reset();
    return ConflictChoice::kSkip;
  }
  return impl_->answer;
}

void ConflictDialogHost::Pump(int language_index) {
  std::optional<Impl::Question> question;
  bool hide = false;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->pending) {
      // 同一个问题、同一个语言：窗口已经是它了，不用每 16ms 重设一遍属性。
      if (impl_->shown_id != impl_->pending->id ||
          impl_->shown_language != language_index)
        question = impl_->pending;
    } else if (impl_->showing) {
      hide = true;
    }
  }
  if (question) {
    Show(question->destination, question->size, question->id, language_index);
  } else if (hide) {
    Hide();
  }
}

void ConflictDialogHost::Show(const std::string& destination, uint64_t size,
                             uint64_t request_id, int language_index) {
  const auto text = [language_index](const char* key, const char* fallback) {
    const auto& localized = localization::Literal(key, language_index);
    return slint::SharedString(localized.empty() ? std::string(fallback) : localized);
  };
  if (!impl_->window) {
    auto window = ui::ConflictDialog::create();
    // 按钮回调只认「第几个按钮」，要不要作用于整批由窗口上的 apply-all 决定。
    (*window).on_chosen([this](int index) {
      const auto base = BaseChoice(index);
      if (!base) return;
      bool all = false;
      if (impl_->window) all = (**impl_->window).get_apply_all();
      Resolve(all ? AllVariant(*base) : *base);
    });
    // Esc 是「跳过这个文件」，不是「全部跳过」。
    (*window).on_dismissed([this]() { Resolve(ConflictChoice::kSkip); });
    impl_->window.emplace(std::move(window));
  }
  auto& dialog = **impl_->window;
  const auto name = FileNameOf(destination);
  const auto folder = FolderOf(destination);
  dialog.set_heading(text("覆盖同名文件", "File already exists"));
  dialog.set_message(text("接收目录里已经有一个同名文件。请选择如何处理这一次接收。",
                          "A file with this name is already in the receive folder. "
                          "Choose what to do with this transfer."));
  dialog.set_file_name(slint::SharedString(name));
  dialog.set_file_size(slint::SharedString(FormatSize(size)));
  dialog.set_folder(slint::SharedString(folder));
  dialog.set_skip_text(text("跳过", "Skip"));
  dialog.set_overwrite_text(text("覆盖", "Overwrite"));
  dialog.set_rename_text(text("重命名", "Rename"));
  dialog.set_apply_all_text(text("全部应用", "Apply to all"));
  // 每个新问题都从未勾选开始：latch 只在用户这一次明确打开时生效。
  dialog.set_apply_all(false);
  dialog.show();
  std::lock_guard lock(impl_->mutex);
  impl_->shown_id = request_id;
  impl_->shown_language = language_index;
  impl_->showing = true;
}

void ConflictDialogHost::Hide() {
  if (impl_->window) (**impl_->window).hide();
  std::lock_guard lock(impl_->mutex);
  impl_->showing = false;
  impl_->shown_id = 0;
}

void ConflictDialogHost::Resolve(Choice choice) {
  {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->pending) return;
    // 「全部应用」记住的是本次批量，不是这台机器：批次结束就清掉。
    if (IsBatchChoice(choice)) {
      if (impl_->batches.size() >= kMaxBatches) impl_->batches.erase(impl_->batches.begin());
      impl_->batches[impl_->pending->remote_id] = choice;
    }
    impl_->answer = choice;
    impl_->answer_id = impl_->pending->id;
    impl_->pending.reset();
  }
  impl_->answered.notify_all();
}

void ConflictDialogHost::EndBatch(const std::string& remote_id) {
  std::lock_guard lock(impl_->mutex);
  impl_->batches.erase(remote_id);
}

std::vector<std::string> ConflictDialogHost::BatchRemotes() const {
  std::lock_guard lock(impl_->mutex);
  std::vector<std::string> remotes;
  remotes.reserve(impl_->batches.size());
  for (const auto& [remote_id, choice] : impl_->batches) {
    (void)choice;
    remotes.push_back(remote_id);
  }
  return remotes;
}

void ConflictDialogHost::Shutdown() {
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopped && !impl_->window) return;
    impl_->stopped = true;
    impl_->pending.reset();
    impl_->batches.clear();
  }
  impl_->answered.notify_all();
  if (impl_->window) {
    (**impl_->window).hide();
    // 关窗必须在 UI 线程；管理器在 Shutdown 时已经不再泵任何东西，句柄随
    // impl_ 一起销毁，Slint 会把它的窗口一并收掉。
    impl_->window.reset();
  }
}

uint64_t ConflictDialogHost::PendingRequest() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->pending ? impl_->pending->id : 0;
}

}  // namespace crossdesk
