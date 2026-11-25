/*
 * 手机端传输面板三个 op 的回归检查。
 *
 * 走和 GUI / 对端层同一个入口（FileBrowser::Handle 加 send 回调），检查
 * transfer_list / transfer_control / transfer_settings：op 表与动作词表的闭合、
 * 没有注入控件时的 unsupported、行与汇总的原样透传（含「应答里不出现控制器
 * 自己的路径」）、八个动作各到达宿主一次、以及设置的类型/范围/边界取整。
 *
 * 控制器库不依赖 GUI，所以这里用一个 StubControl 实现 TransferControl：
 * 桌面那边由 FileTransferManager 的适配器接入（见 file_browser.h 的注入点）。
 */

#include <file_browser.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using crossdesk::FileBrowser;
using crossdesk::TransferControl;
using crossdesk::TransferRowView;
using crossdesk::TransferSummaryView;
namespace ops = crossdesk::file_browser_ops;

namespace {

int g_failures = 0;

void Expect(bool condition, const char* what) {
  if (condition) return;
  std::printf("FAIL  %s\n", what);
  ++g_failures;
}

std::string Utf8(const fs::path& path) {
  const auto text = path.u8string();
  return {text.begin(), text.end()};
}

fs::path ScratchRoot(const std::string& tag) {
  std::error_code ec;
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root = fs::temp_directory_path(ec) /
                    ("crossdesk-file-browser-transfer-" + tag + "-" + std::to_string(stamp));
  fs::create_directories(root, ec);
  return root;
}

// 桩宿主：每次被调用记一笔，返回值可控。测试靠它在「宿主那一侧」观察 op 到底
// 转发了什么，而不需要一个 GUI。
struct StubControl final : TransferControl {
  std::vector<TransferRowView> rows;
  TransferSummaryView summary;
  int rate = 0, policy = 2, concurrency = 2;
  bool answer = true;
  std::vector<std::string> calls;
  std::string last_remote;
  uint32_t last_id = 0;

  void ListTransfers(const std::string& remote, std::vector<TransferRowView>* out,
                     TransferSummaryView* total) override {
    calls.push_back("list");
    last_remote = remote;
    if (out) *out = rows;
    if (total) *total = summary;
  }
  bool Pause(const std::string& remote, uint32_t id) override {
    return Record("pause", remote, id);
  }
  bool Resume(const std::string& remote, uint32_t id) override {
    return Record("resume", remote, id);
  }
  bool Cancel(const std::string& remote, uint32_t id) override {
    return Record("cancel", remote, id);
  }
  bool Retry(const std::string& remote, uint32_t id) override {
    return Record("retry", remote, id);
  }
  bool StartAll(const std::string& remote) override { return Record("start_all", remote, 0); }
  bool PauseAll(const std::string& remote) override { return Record("pause_all", remote, 0); }
  bool ResumeAll(const std::string& remote) override { return Record("resume_all", remote, 0); }
  bool ClearAll(const std::string& remote) override { return Record("clear_all", remote, 0); }
  bool RemoveFinished(const std::string& remote) override {
    return Record("remove_finished", remote, 0);
  }
  void SetRateKbps(int value) override {
    calls.push_back("set_rate");
    rate = value;
  }
  int RateKbps() override {
    calls.push_back("get_rate");
    return rate;
  }
  void SetOverwritePolicy(int value) override {
    calls.push_back("set_policy");
    policy = value;
  }
  int OverwritePolicy() override {
    calls.push_back("get_policy");
    return policy;
  }
  void SetConcurrency(int value) override {
    calls.push_back("set_concurrency");
    concurrency = value;
  }
  int Concurrency() override {
    calls.push_back("get_concurrency");
    return concurrency;
  }

  bool Record(const char* name, const std::string& remote, uint32_t id) {
    calls.push_back(name);
    last_remote = remote;
    last_id = id;
    return answer;
  }
  std::size_t Count(const std::string& name) const {
    std::size_t total = 0;
    for (const auto& call : calls) total += call == name ? 1u : 0u;
    return total;
  }
  void Reset() {
    calls.clear();
    last_remote.clear();
    last_id = 0;
  }
};

// A peer with no transport. The send callback records what the dispatcher
// answered and Call() waits for the matching id, which is exactly how the GUI and
// peer layers consume replies.
struct Peer {
  std::mutex mutex;
  std::condition_variable changed;
  std::map<uint32_t, nlohmann::json> replies;
  uint32_t next_id = 1;
  uint32_t last_id = 0;
  FileBrowser browser;

  Peer()
      : browser(
            [this](const std::string&, const nlohmann::json& message) {
              {
                std::lock_guard lock(mutex);
                if (message.contains("id") && message["id"].is_number_unsigned())
                  replies[message["id"].get<uint32_t>()] = message;
              }
              changed.notify_all();
              return true;
            },
            [](const std::string&, const std::string&, uint32_t) { return true; },
            [](const std::string&, uint32_t, const fs::path&) { return true; },
            [](const std::string&, uint32_t) { return true; }) {}

  nlohmann::json Call(const std::string& op,
                      nlohmann::json fields = nlohmann::json::object()) {
    const auto id = next_id++;
    last_id = id;
    fields["type"] = "file_browser";
    fields["version"] = 1;
    fields["id"] = id;
    fields["op"] = op;
    browser.Handle("peer", std::move(fields));
    std::unique_lock lock(mutex);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    if (!changed.wait_until(lock, deadline, [this, id] { return replies.count(id) != 0; }))
      return nlohmann::json::object();
    return replies[id];
  }

  // 每条应答都必须留在 version 1 信封里、回显 id，并带一个布尔 ok：
  // 手机端只依赖这几件事，所以每个用例都过一遍。
  void ExpectEnvelope(const nlohmann::json& reply, const char* what) {
    Expect(reply.value("op", std::string{}) == "reply", what);
    Expect(reply.value("id", 0u) == last_id, "the reply echoes the request id");
    Expect(reply.value("type", std::string{}) == "file_browser", "the reply keeps the message type");
    Expect(reply.value("version", 0) == 1, "the reply keeps the wire version");
    Expect(reply.contains("ok") && reply.at("ok").is_boolean(), "the reply carries a boolean ok");
  }
};

// -------------------------------------------------------------- vocabulary --
void TestOpTable() {
  Expect(ops::kOps.size() == 18, "the op table lists every dispatched op");
  Expect(ops::kFileManagementOps.size() == 11, "eleven file management ops were added");
  for (const auto name : {ops::kOpTransferList, ops::kOpTransferControl, ops::kOpTransferSettings}) {
    Expect(ops::IsOpName(name), "a transfer panel op is in the dispatch table");
    Expect(ops::IsFileManagementOp(name), "a transfer panel op is flagged as new");
  }
  for (const auto name :
       {"list", "gallery", "thumb", "list_cancel", "prepare", "cancel", "copy"})
    Expect(!ops::IsFileManagementOp(name), "a pre-existing op is not flagged as new");
  std::size_t flagged = 0;
  for (const auto& info : ops::kOps) flagged += info.file_management ? 1 : 0;
  Expect(flagged == ops::kFileManagementOps.size(), "the table flags exactly the new ops");
  Expect(!ops::IsOpName("transfer_delete") && !ops::IsOpName(""),
         "an op outside the table is never accepted");

  Expect(ops::kTransferActions.size() == 9, "the action vocabulary has nine entries");
  for (const auto action : ops::kTransferActions)
    Expect(ops::IsTransferAction(action), "a listed action is accepted");
  Expect(!ops::IsTransferAction("kill") && !ops::IsTransferAction("") &&
             !ops::IsTransferAction("Pause") && !ops::IsTransferAction("restart"),
         "an action outside the closed list is never accepted");
  Expect(ops::kTransferItemActions.size() == 4, "four actions are per item");
  for (const auto action : {"pause", "resume", "retry"})
    Expect(ops::IsTransferItemAction(action), "pause/resume/retry are per item");
  for (const auto action :
       {"start_all", "pause_all", "resume_all", "clear_all", "remove_finished"})
    Expect(!ops::IsTransferItemAction(action), "a batch action is not per item");

  Expect(ops::kTransferStateNames.size() == 6, "six row states are on the wire");
  for (const auto state : {"queued", "sending", "paused", "completed", "failed", "cancelled"})
    Expect(ops::IsTransferState(state), "a documented row state is accepted");
  Expect(!ops::IsTransferState("idle") && !ops::IsTransferState(""),
         "an undocumented row state is not");

  Expect(ops::kTransferRateKbpsMin == 0 && ops::kTransferRateKbpsMax == 1048576 &&
             ops::kTransferOverwritePolicyMin == 0 && ops::kTransferOverwritePolicyMax == 2 &&
             ops::kTransferConcurrencyMin == 1 && ops::kTransferConcurrencyMax == 4,
         "the settings ranges match config_center and the queue ceiling");
  for (const auto code : {"invalid_request", "unsupported"})
    Expect(ops::IsErrorCode(code), "the transfer ops only need existing codes");
}

// ---------------------------------------------------------- no injection --
void TestNoControl(Peer& peer) {
  peer.browser.SetTransferControl(nullptr);
  auto reply = peer.Call("transfer_list");
  peer.ExpectEnvelope(reply, "transfer_list answers with the reply envelope");
  Expect(!reply.value("ok", true), "transfer_list without a control is refused");
  Expect(reply.value("error", std::string{}) == "unsupported",
         "transfer_list without a control answers unsupported");

  reply = peer.Call("transfer_control", {{"action", "pause"}, {"file_id", 1}});
  Expect(reply.value("error", std::string{}) == "unsupported",
         "transfer_control without a control answers unsupported");
  reply = peer.Call("transfer_settings");
  Expect(reply.value("error", std::string{}) == "unsupported",
         "transfer_settings without a control answers unsupported");
  // 校验先于能力探测：坏请求在任何宿主上都是 invalid_request。
  reply = peer.Call("transfer_control", {{"action", "kill"}});
  Expect(reply.value("error", std::string{}) == "invalid_request",
         "a malformed action is invalid_request even without a control");
}

// ------------------------------------------------------------------- list --
void TestList(Peer& peer, StubControl& control, const fs::path& receive) {
  control.rows = {
      {7, "photo.jpg", "C:\\incoming\\photo.jpg", 4096, 1024, 512, "sending", false, false, false},
      {9, "报告.pdf", "C:\\incoming\\报告.pdf", 8192, 0, 0, "queued", true, true, true}};
  control.summary.total = 2;
  control.summary.completed = 1;
  control.summary.failed = 0;
  control.summary.active = 1;
  control.summary.queued = 0;
  control.summary.paused = 1;
  control.summary.batch_partial = true;
  // 接收目录只有控制器自己知道：它绝不能出现在应答里。
  peer.browser.SetReceiveDirectory(Utf8(receive));
  control.Reset();

  auto reply = peer.Call("transfer_list", {{"remote", "peer-a"}});
  peer.ExpectEnvelope(reply, "transfer_list answers with the reply envelope");
  Expect(reply.value("ok", false), "transfer_list reports the host rows");
  Expect(control.Count("list") == 1, "transfer_list reaches the control exactly once");
  Expect(control.last_remote == "peer", "the remote field is passed through");

  const auto& rows = reply.at("rows");
  Expect(rows.is_array() && rows.size() == 2, "both rows come back");
  if (rows.is_array() && rows.size() == 2) {
    Expect(rows[0].value("id", 0u) == 7u, "the row id passes through");
    Expect(rows[0].value("name", std::string{}) == "photo.jpg", "the row name passes through");
    Expect(rows[0].value("path", std::string{}) == control.rows[0].path,
           "the row path passes through unchanged");
    Expect(rows[0].value("size", 0ull) == 4096ull && rows[0].value("sent", 0ull) == 1024ull,
           "the byte counters pass through");
    Expect(rows[0].value("rate", 0u) == 512u, "the rate passes through");
    Expect(rows[0].value("state", std::string{}) == "sending", "the row state passes through");
    Expect(!rows[0].value("paused", true) && !rows[0].value("retryable", true) &&
               !rows[0].value("receiving", true),
           "the row flags pass through");
    Expect(rows[1].value("paused", false) && rows[1].value("retryable", false) &&
               rows[1].value("receiving", false),
           "a held, retryable, receiving row keeps all three flags");
    Expect(rows[1].value("name", std::string{}) == "报告.pdf", "a non-ASCII name survives");
    Expect(rows[0].size() == 10, "a row carries exactly the documented fields");
  }
  const auto& summary = reply.at("summary");
  Expect(summary.value("total", 0ull) == 2ull && summary.value("completed", 0ull) == 1ull &&
             summary.value("failed", 0ull) == 0ull && summary.value("active", 0ull) == 1ull &&
             summary.value("queued", 0ull) == 0ull && summary.value("paused", 0ull) == 1ull,
         "the batch counters pass through");
  Expect(summary.value("batch_partial", false), "the partial success flag passes through");
  Expect(summary.size() == 7, "the summary carries exactly the documented fields");

  const auto dump = reply.dump();
  Expect(dump.find(Utf8(receive)) == std::string::npos,
         "the reply never carries a controller path the host did not supply");

  // 队列作用域永远是连接 id：请求体里的 remote 只是一个提示，缺省、写成数字、
  // 或者写成别的会话 id，都不会改变控制器看到的作用域（越权入口就是这样堵住的）。
  control.Reset();
  reply = peer.Call("transfer_list");
  Expect(reply.value("ok", false) && control.last_remote == "peer",
         "the queue scope comes from the connection, not the payload");
  reply = peer.Call("transfer_list", {{"remote", 7}});
  Expect(reply.value("ok", false) && control.last_remote == "peer",
         "a non-string remote does not change the scope");
  reply = peer.Call("transfer_list", {{"remote", "other-session"}});
  Expect(reply.value("ok", false) && control.last_remote == "peer",
         "another session id in the payload cannot steal the scope");
  Expect(control.Count("list") == 3,
         "every list request reaches the control under the connection scope");
}

// ---------------------------------------------------------------- control --
void TestControlActions(Peer& peer, StubControl& control) {
  struct Case {
    const char* action;
    uint32_t id;
  };
  const Case cases[] = {{"pause", 7},      {"resume", 7},     {"retry", 7},
                        {"start_all", 0},  {"pause_all", 0},  {"resume_all", 0},
                        {"clear_all", 0},  {"remove_finished", 0}};
  for (const auto& item : cases) {
    control.Reset();
    nlohmann::json fields{{"action", item.action}};
    if (item.id) fields["file_id"] = item.id;
    const auto reply = peer.Call("transfer_control", fields);
    peer.ExpectEnvelope(reply, "transfer_control answers with the reply envelope");
    Expect(reply.value("ok", false), "a known action is accepted");
    Expect(reply.value("action", std::string{}) == item.action, "the reply echoes the action");
    Expect(reply.value("applied", false), "the host answer comes back as applied");
    Expect(control.calls.size() == 1, "each action reaches the control exactly once");
    Expect(control.Count(item.action) == 1, "the control saw exactly that action");
    if (item.id) Expect(control.last_id == item.id, "the item id reaches the control");
  }

  // 宿主说「没做」也是 ok:true + applied:false：请求本身是合法的。
  control.answer = false;
  control.Reset();
  auto reply = peer.Call("transfer_control", {{"action", "retry"}, {"file_id", 3}});
  Expect(reply.value("ok", false) && !reply.value("applied", true),
         "an unapplied action is still a well formed reply");
  control.answer = true;

  // 词表闭合 + id 规则：全都停在控制器里，一次也不落到宿主。
  control.Reset();
  for (const auto bad : {"kill", "", "Pause", "start"}) {
    reply = peer.Call("transfer_control", {{"action", bad}});
    Expect(!reply.value("ok", true), "an unknown action is refused");
    Expect(reply.value("error", std::string{}) == "invalid_request",
           "an unknown action answers invalid_request");
  }
  reply = peer.Call("transfer_control", {{"action", "pause"}});
  Expect(reply.value("error", std::string{}) == "invalid_request",
         "a per-item action without an id is invalid_request");
  for (const auto bad_id : {0, -1}) {
    reply = peer.Call("transfer_control", {{"action", "resume"}, {"file_id", bad_id}});
    Expect(reply.value("error", std::string{}) == "invalid_request",
           "an id that is not a positive integer is invalid_request");
  }
  reply = peer.Call("transfer_control", {{"action", "pause"}, {"file_id", "7"}});
  Expect(reply.value("error", std::string{}) == "invalid_request",
         "a string id is invalid_request");
  reply = peer.Call("transfer_control", nlohmann::json::object());
  Expect(reply.value("error", std::string{}) == "invalid_request",
         "a missing action is invalid_request");
  Expect(control.calls.empty(), "no malformed request reached the control");
}

// --------------------------------------------------------------- settings --
void TestSettings(Peer& peer, StubControl& control) {
  control.rate = 128;
  control.policy = 1;
  control.concurrency = 3;
  control.Reset();
  auto reply = peer.Call("transfer_settings");
  peer.ExpectEnvelope(reply, "transfer_settings answers with the reply envelope");
  Expect(reply.value("ok", false), "transfer_settings without fields is a GET");
  Expect(reply.value("rate_kbps", -1) == 128, "the GET reports the host rate");
  Expect(reply.value("overwrite_policy", -1) == 1, "the GET reports the host policy");
  Expect(reply.value("concurrency", -1) == 3, "the GET reports the host concurrency");
  Expect(control.Count("set_rate") == 0 && control.Count("set_policy") == 0 &&
             control.Count("set_concurrency") == 0,
         "a GET changes nothing");

  // 区间端点必须被接受：0 与 1048576、0 与 2、1 与 4。
  reply = peer.Call("transfer_settings", {{"rate_kbps", 0}});
  Expect(reply.value("ok", false) && reply.value("rate_kbps", -1) == 0 && control.rate == 0,
         "rate_kbps 0 is applied");
  reply = peer.Call("transfer_settings", {{"rate_kbps", 1048576}});
  Expect(reply.value("ok", false) && reply.value("rate_kbps", -1) == 1048576 &&
             control.rate == 1048576,
         "rate_kbps at the ceiling is applied");
  reply = peer.Call("transfer_settings", {{"overwrite_policy", 0}});
  Expect(reply.value("ok", false) && reply.value("overwrite_policy", -1) == 0 &&
             control.policy == 0,
         "overwrite_policy 0 is applied");
  reply = peer.Call("transfer_settings", {{"overwrite_policy", 2}});
  Expect(reply.value("ok", false) && reply.value("overwrite_policy", -1) == 2 &&
             control.policy == 2,
         "overwrite_policy 2 is applied");
  reply = peer.Call("transfer_settings", {{"concurrency", 1}});
  Expect(reply.value("ok", false) && reply.value("concurrency", -1) == 1 &&
             control.concurrency == 1,
         "concurrency 1 is applied");
  reply = peer.Call("transfer_settings", {{"concurrency", 4}});
  Expect(reply.value("ok", false) && reply.value("concurrency", -1) == 4 &&
             control.concurrency == 4,
         "concurrency 4 is applied");

  // 越界、类型不对、字段不认识：一律 invalid_request，且一次也不落到宿主。
  control.Reset();
  const auto Reject = [&peer, &control](nlohmann::json fields, const char* what) {
    const auto refused = peer.Call("transfer_settings", std::move(fields));
    Expect(!refused.value("ok", true), what);
    Expect(refused.value("error", std::string{}) == "invalid_request", what);
  };
  Reject({{"rate_kbps", 1048577}}, "rate_kbps above the ceiling is invalid_request");
  Reject({{"rate_kbps", -1}}, "a negative rate_kbps is invalid_request");
  Reject({{"rate_kbps", "64"}}, "a string rate_kbps is invalid_request");
  Reject({{"overwrite_policy", 3}}, "overwrite_policy above the range is invalid_request");
  Reject({{"overwrite_policy", "1"}}, "a string overwrite_policy is invalid_request");
  Reject({{"concurrency", 0}}, "concurrency below the range is invalid_request");
  Reject({{"concurrency", 5}}, "concurrency above the range is invalid_request");
  Reject({{"concurrency", "2"}}, "a string concurrency is invalid_request");
  Reject({{"bitrate", 10}}, "an unknown field is invalid_request");
  Reject({{"rate_kbps", 64}, {"overwrite", 1}},
         "one unknown field refuses the whole request");
  Expect(control.calls.empty(), "no invalid settings request reached the host");

  // 宿主回一个越界配置时，应答仍然落在合法区间内（回读也再夹一次）。
  control.rate = 99999999;
  control.policy = 9;
  control.concurrency = 0;
  reply = peer.Call("transfer_settings");
  Expect(reply.value("ok", false) && reply.value("rate_kbps", -1) == 1048576,
         "a host rate above the ceiling is clamped in the reply");
  Expect(reply.value("overwrite_policy", -1) == 2,
         "a host policy above the range is clamped in the reply");
  Expect(reply.value("concurrency", -1) == 1,
         "a host concurrency below the range is clamped in the reply");
}

}  // namespace

int main() {
  const auto root = ScratchRoot("root");
  Expect(!root.empty() && fs::is_directory(root), "the scratch root exists");
  const auto receive = root / "receive";
  fs::create_directories(receive);

  TestOpTable();
  // 桩比浏览器活得久：注入点指向它，所以它的生命周期必须包住 Peer。
  StubControl control;
  {
    Peer peer;
    TestNoControl(peer);
    peer.browser.SetTransferControl(&control);
    TestList(peer, control, receive);
    TestControlActions(peer, control);
    TestSettings(peer, control);
    peer.browser.SetTransferControl(nullptr);
  }

  std::error_code ec;
  fs::remove_all(root, ec);
  if (g_failures) {
    std::printf("%d file browser transfer op checks failed\n", g_failures);
    return 1;
  }
  std::printf("file browser transfer ops checks passed\n");
  return 0;
}
