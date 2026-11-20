#pragma once

// Flight recorder + Command pattern for the map editor.
// Every mutating action should go through CommandManager so a SEH crash can dump
// the last N commands next to the minidump (see main.cpp omsi_unhandled_exception).
//
// Undo/redo of map state remains on EditorSnapshot (mod_undo.inc.cpp). Commands here
// are for: (1) pre-execution logging, (2) optional scripted/test dispatch by name,
// (3) gradual migration of actions into Execute()/Undo() objects.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace omsi::editor {

class Command {
public:
    virtual ~Command() = default;
    virtual void Execute() = 0;
    // Prefer empty Undo when the action already called push_undo() (snapshot system).
    virtual void Undo() {}
    [[nodiscard]] virtual std::string GetName() const = 0;
    [[nodiscard]] virtual std::string GetDetail() const { return {}; }
    [[nodiscard]] std::chrono::system_clock::time_point GetTimestamp() const { return timestamp_; }

protected:
    std::chrono::system_clock::time_point timestamp_ = std::chrono::system_clock::now();
};

// Wraps any existing editor mutation without rewriting it as a class hierarchy.
class LambdaCommand final : public Command {
public:
    LambdaCommand(std::string name, std::function<void()> exec, std::function<void()> undo = {},
                  std::string detail = {})
        : name_(std::move(name)), detail_(std::move(detail)), exec_(std::move(exec)),
          undo_(std::move(undo)) {}

    void Execute() override {
        if (exec_) exec_();
    }
    void Undo() override {
        if (undo_) undo_();
    }
    [[nodiscard]] std::string GetName() const override { return name_; }
    [[nodiscard]] std::string GetDetail() const override { return detail_; }

private:
    std::string name_;
    std::string detail_;
    std::function<void()> exec_;
    std::function<void()> undo_;
};

[[nodiscard]] inline std::unique_ptr<Command> make_lambda_command(std::string name,
                                                                  std::function<void()> exec,
                                                                  std::string detail = {}) {
    return std::make_unique<LambdaCommand>(std::move(name), std::move(exec), std::function<void()>{},
                                           std::move(detail));
}

struct CommandBreadcrumb {
    std::chrono::system_clock::time_point time = std::chrono::system_clock::now();
    std::string name;
    std::string detail;
    std::uint64_t seq = 0;
};

class CommandManager {
public:
    static CommandManager& Get() {
        static CommandManager instance;
        return instance;
    }

    void InitializeLog(const std::string& log_file_path) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (log_file_.is_open()) log_file_.close();
        log_path_ = log_file_path;
        log_file_.open(log_file_path, std::ios::out | std::ios::app);
        log_unlocked_("--- Command Log Initialized ---");
    }

    // Record without owning a Command (hot path: push_undo, shortcut edge, panel flags).
    void Note(std::string_view name, std::string_view detail = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        note_unlocked_(name, detail);
    }

    // Execute + breadcrumb. Logs BEFORE Execute so a crash mid-command is attributable.
    void ExecuteCommand(std::unique_ptr<Command> command) {
        if (!command) return;
        std::string name;
        std::string detail;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            name = command->GetName();
            detail = command->GetDetail();
            note_unlocked_(name, detail);
        }
        command->Execute();
        std::lock_guard<std::mutex> lock(mutex_);
        history_.push_back(std::move(command));
        if (history_.size() > max_history_) history_.pop_front();
    }

    void UndoLastCommandObject() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (history_.empty()) return;
        auto& last = history_.back();
        note_unlocked_("UNDO_CMD", last->GetName());
        last->Undo();
        history_.pop_back();
    }

    // Named handlers for crash-test / CLI replay (register once at editor boot).
    void RegisterHandler(std::string name, std::function<void()> handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        handlers_[std::move(name)] = std::move(handler);
    }

    bool RunByName(std::string_view name) {
        std::function<void()> fn;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = handlers_.find(std::string(name));
            if (it == handlers_.end() || !it->second) return false;
            fn = it->second;
            note_unlocked_(std::string("RUN:") + std::string(name), "handler");
        }
        fn();
        return true;
    }

    [[nodiscard]] std::vector<std::string> ListHandlers() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> out;
        out.reserve(handlers_.size());
        for (const auto& [k, _] : handlers_) out.push_back(k);
        return out;
    }

    void DumpHistoryToFile(const std::string& dump_path) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ofstream out(dump_path, std::ios::out | std::ios::trunc);
        if (!out.is_open()) return;
        out << "=== COMMAND BREADCRUMBS AT CRASH (last " << crumbs_.size() << ") ===\n";
        out << "log_path=" << log_path_ << "\n";
        out << "seq_last=" << seq_ << "\n";
        for (const auto& c : crumbs_) {
            const auto tt = std::chrono::system_clock::to_time_t(c.time);
            std::tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &tt);
#else
            localtime_r(&tt, &tm);
#endif
            out << '#' << c.seq << ' ' << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << " | " << c.name;
            if (!c.detail.empty()) out << " | " << c.detail;
            out << '\n';
        }
        out << "=== OWNED COMMAND OBJECTS (" << history_.size() << ") ===\n";
        for (const auto& cmd : history_) {
            out << cmd->GetName();
            const auto d = cmd->GetDetail();
            if (!d.empty()) out << " | " << d;
            out << '\n';
        }
        out.flush();
    }

    // Safe to call from SEH filter (best-effort; may fail if heap is corrupt).
    void DumpHistoryBestEffort(const char* dump_path) {
        try {
            DumpHistoryToFile(dump_path ? dump_path : "crash_command_history.txt");
        } catch (...) {
        }
    }

    [[nodiscard]] std::string LastCommandName() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (crumbs_.empty()) return {};
        return crumbs_.back().name;
    }

    void set_max_crumbs(std::size_t n) {
        std::lock_guard<std::mutex> lock(mutex_);
        max_crumbs_ = n ? n : 1;
        while (crumbs_.size() > max_crumbs_) crumbs_.pop_front();
    }

private:
    CommandManager() = default;
    ~CommandManager() {
        if (log_file_.is_open()) log_file_.close();
    }

    void note_unlocked_(std::string_view name, std::string_view detail) {
        CommandBreadcrumb c;
        c.seq = ++seq_;
        c.name.assign(name.begin(), name.end());
        c.detail.assign(detail.begin(), detail.end());
        crumbs_.push_back(std::move(c));
        if (crumbs_.size() > max_crumbs_) crumbs_.pop_front();

        std::ostringstream ss;
        ss << "[CMD #" << seq_ << "] " << name;
        if (!detail.empty()) ss << " | " << detail;
        log_unlocked_(ss.str());
    }

    void log_unlocked_(const std::string& msg) {
        if (!log_file_.is_open()) return;
        const auto tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &tt);
#else
        localtime_r(&tt, &tm);
#endif
        log_file_ << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << " | " << msg << '\n';
        log_file_.flush();
    }

    mutable std::mutex mutex_;
    std::ofstream log_file_;
    std::string log_path_;
    std::deque<std::unique_ptr<Command>> history_;
    std::deque<CommandBreadcrumb> crumbs_;
    std::unordered_map<std::string, std::function<void()>> handlers_;
    std::uint64_t seq_ = 0;
    std::size_t max_history_ = 256;
    std::size_t max_crumbs_ = 1000;
};

// Convenience aliases matching the design names from the original request.
using InstallCrashHandlerHook = void (*)();

} // namespace omsi::editor
