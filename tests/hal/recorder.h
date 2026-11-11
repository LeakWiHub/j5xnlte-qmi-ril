// SPDX-License-Identifier: Apache-2.0
// Thread-safe event log shared by the generated test recorders (tests only).
#pragma once
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Recorder {
  public:
    void add(const std::string& s) {
        std::lock_guard<std::mutex> l(mu_);
        log_.push_back(s);
        cv_.notify_all();
    }
    bool wait(const std::string& prefix, int ms = 5000) {
        std::unique_lock<std::mutex> l(mu_);
        return cv_.wait_for(l, std::chrono::milliseconds(ms), [&] { return find(prefix) >= 0; });
    }
    int count(const std::string& prefix) {
        std::lock_guard<std::mutex> l(mu_);
        int n = 0;
        for (auto& e : log_) n += e.rfind(prefix, 0) == 0;
        return n;
    }
    std::string last(const std::string& prefix) {
        std::lock_guard<std::mutex> l(mu_);
        for (size_t i = log_.size(); i-- > 0;)
            if (log_[i].rfind(prefix, 0) == 0) return log_[i];
        return "";
    }

  private:
    int find(const std::string& prefix) {
        for (size_t i = 0; i < log_.size(); i++)
            if (log_[i].rfind(prefix, 0) == 0) return static_cast<int>(i);
        return -1;
    }
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::string> log_;
};
