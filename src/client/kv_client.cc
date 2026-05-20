#include "client/kv_client.h"

#include <brpc/channel.h>
#include <brpc/controller.h>
#include <butil/logging.h>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace raftkv {
// ── 工具函数 ──────────────────────────────────────────────────────────
std::vector<std::string> KVClient::ParsePeers(const std::string &peers) {
  std::vector<std::string> result;
  std::stringstream ss(peers);
  std::string addr;
  while (std::getline(ss, addr, ',')) {
    // braft PeerId 格式为 ip:port:index，普通地址为 ip:port
    // 只有包含两个冒号（三段）时才剥离最后一段
    auto first_colon = addr.find(':');
    auto last_colon = addr.rfind(':');
    if (first_colon != std::string::npos && last_colon != first_colon) {
      std::string suffix = addr.substr(last_colon + 1);
      bool all_digits =
          !suffix.empty() &&
          suffix.find_first_not_of("0123456789") == std::string::npos;
      if (all_digits) {
        addr = addr.substr(0, last_colon);
      }
    }
    if (!addr.empty()) {
      result.push_back(addr);
    }
  }
  return result;
}

bool KVClient::InitChannel(const std::string &addr) {
  brpc::ChannelOptions ch_opts;
  ch_opts.timeout_ms = options_.timeout_ms;
  if (channel_.Init(addr.c_str(), &ch_opts) != 0) {
    LOG(ERROR) << "连接失败: " << addr;
    return false;
  }
  stub_ = std::make_unique<kv::KvService_Stub>(&channel_);
  current_addr_ = addr;
  return true;
}

bool KVClient::HandleRedirect(const std::string &redirect_addr) {
  if (redirect_addr.empty()) {
    return false;
  }
  // braft PeerId 格式 ip:port:index，brpc 只接受 ip:port
  // 按冒号分段，取前两段拼接，丢弃多余的 :index
  std::string addr;
  auto first_colon = redirect_addr.find(':');
  if (first_colon == std::string::npos) {
    addr = redirect_addr; // 没有冒号，直接用
  } else {
    auto second_colon = redirect_addr.find(':', first_colon + 1);
    if (second_colon == std::string::npos) {
      addr = redirect_addr; // 只有一个冒号，已是 ip:port 格式
    } else {
      addr = redirect_addr.substr(0, second_colon); // 截取 ip:port
    }
  }
  LOG(INFO) << "Redirect → " << addr;
  return InitChannel(addr);
}

// ── 构造函数 ──────────────────────────────────────────────────────────
KVClient::KVClient(const std::string &server_addr) {
  options_.peers = server_addr;
  options_.linearizable = false;
  peer_list_ = {server_addr};
  InitChannel(server_addr);
}

KVClient::KVClient(const ClientOptions &options) : options_(options) {
  peer_list_ = ParsePeers(options.peers);
  if (peer_list_.empty()) {
    LOG(ERROR) << "peers 列表为空";
    return;
  }
  // 默认连接第一个节点
  InitChannel(peer_list_[0]);
}

// ── Put ────────────────────────────────────────────────────────────────

bool KVClient::Put(const std::string &key, const std::string &value) {
  for (int retry = 0; retry <= options_.max_retry; retry++) {

    kv::PutRequest req;
    kv::PutResponse resp;
    brpc::Controller cntl;

    req.set_key(key);
    req.set_value(value);
    stub_->Put(&cntl, &req, &resp, nullptr);

    if (cntl.Failed()) {
      LOG(ERROR) << "Put RPC 失败: " << cntl.ErrorText();
      if (retry < options_.max_retry && !peer_list_.empty()) {
        int next = (retry + 1) % static_cast<int>(peer_list_.size());
        InitChannel(peer_list_[next]);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        continue;
      }
      return false;
    }
    if (resp.success()) {
      return true;
    }
    if (!resp.redirect().empty()) {
      if (HandleRedirect(resp.redirect())) {
        continue;
      }
    }
    if (resp.error() == "no leader") {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    LOG(WARNING) << "Put 失败: " << resp.error();
    return false;
  }
  return false;
}

bool KVClient::Get(const std::string &key, std::string *value_out,
                   bool *found_out) {
  for (int retry = 0; retry <= options_.max_retry; ++retry) {
    kv::GetRequest req;
    kv::GetResponse resp;
    brpc::Controller cntl;

    req.set_key(key);
    req.set_linearizable(options_.linearizable);
    stub_->Get(&cntl, &req, &resp, nullptr);

    if (cntl.Failed()) {
      LOG(ERROR) << "Get RPC 失败: " << cntl.ErrorText();
      if (retry < options_.max_retry && !peer_list_.empty()) {
        int next = (retry + 1) % static_cast<int>(peer_list_.size());
        InitChannel(peer_list_[next]);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        continue;
      }
      return false;
    }
    if (resp.success()) {
      *found_out = resp.found();
      if (resp.found()) {
        *value_out = resp.value();
      }
      return true;
    }

    if (!resp.redirect().empty()) {
      if (HandleRedirect(resp.redirect())) {
        continue;
      }
    }
    if (resp.error() == "no leader") {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    LOG(WARNING) << "Get 失败: " << resp.error();
    return false;
  }
  return false;
}

bool KVClient::Delete(const std::string &key) {
  for (int retry = 0; retry <= options_.max_retry; ++retry) {
    kv::DeleteRequest req;
    kv::DeleteResponse resp;
    brpc::Controller cntl;

    req.set_key(key);
    stub_->Delete(&cntl, &req, &resp, nullptr);

    if (cntl.Failed()) {
      LOG(ERROR) << "Delete RPC 失败: " << cntl.ErrorText();
      if (retry < options_.max_retry && !peer_list_.empty()) {
        int next = (retry + 1) % static_cast<int>(peer_list_.size());
        InitChannel(peer_list_[next]);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        continue;
      }
      return false;
    }

    if (resp.success())
      return true;

    if (!resp.redirect().empty()) {
      if (HandleRedirect(resp.redirect()))
        continue;
    }
    if (resp.error() == "no leader") {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    LOG(WARNING) << "Delete 失败: " << resp.error();
    return false;
  }
  return false;
}

bool KVClient::Scan(const std::string &start_key, const std::string &end_key,
                    int limit,
                    std::vector<std::pair<std::string, std::string>> *kvs_out) {
  for (int retry = 0; retry <= options_.max_retry; ++retry) {
    kv::ScanRequest req;
    kv::ScanResponse resp;
    brpc::Controller cntl;

    req.set_start_key(start_key);
    req.set_end_key(end_key);
    req.set_limit(limit);
    req.set_linearizable(options_.linearizable);
    stub_->Scan(&cntl, &req, &resp, nullptr);

    if (cntl.Failed()) {
      LOG(ERROR) << "Scan RPC 失败: " << cntl.ErrorText();
      if (retry < options_.max_retry && !peer_list_.empty()) {
        int next = (retry + 1) % static_cast<int>(peer_list_.size());
        InitChannel(peer_list_[next]);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        continue;
      }
      return false;
    }

    if (resp.success()) {
      kvs_out->clear();
      for (const auto &kv : resp.kvs()) {
        kvs_out->emplace_back(kv.key(), kv.value());
      }
      return true;
    }

    if (!resp.redirect().empty()) {
      if (HandleRedirect(resp.redirect()))
        continue;
    }
    if (resp.error() == "no leader") {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    LOG(WARNING) << "Scan 失败: " << resp.error();
    return false;
  }
  return false;
}

} // namespace raftkv
