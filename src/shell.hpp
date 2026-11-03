#ifndef TERMINATOR_SHELL_HPP
#define TERMINATOR_SHELL_HPP

#include "common.hpp"
#include "parser.hpp"
#include "jobs.hpp"
#include "history.hpp"
#include "multiwatch.hpp"
#include <string>
#include <vector>
#include <functional>
#include <atomic>

namespace Terminator {

    /**
     * ShellEngine executes commands, manages current directory, job control,
     * environment variables, and builtins for a single tab.
     */
    class ShellEngine {
    public:
        using OutputCallback = std::function<void(const std::string&)>;

        ShellEngine(HistoryManager* history_mgr, OutputCallback out_cb, int in_fd = -1, int out_fd = -1);
        ~ShellEngine();

        /**
         * Execute a complete command line.
         * Handles builtins, pipelines, redirections, background jobs, and multiWatch.
         */
        void execute(const std::string& command_line);

        /**
         * Handle Ctrl+C signal from GUI for this tab.
         */
        void handleCtrlC();

        /**
         * Handle Ctrl+Z signal from GUI for this tab.
         */
        void handleCtrlZ();

        /**
         * Poll job statuses and output any completion notifications.
         */
        void pollJobs();

        /**
         * Current working directory of this tab.
         */
        std::string getCwd() const { return cwd_; }

        /**
         * Tab title (shortened path: e.g. ~/Downloads).
         */
        std::string getTitle() const { return shortenPath(cwd_); }

        /**
         * Check if a foreground process is currently running.
         */
        bool isForegroundRunning() const { return fg_pgid_ > 0; }

        /**
         * Last command exit status ($?).
         */
        int getLastExitStatus() const { return last_exit_status_; }

        /**
         * Job manager reference.
         */
        JobManager& getJobManager() { return jobs_; }

    private:
        // Built-in command handlers
        bool handleBuiltin(const Command& cmd, const std::string& raw_line);
        void builtinCd(const std::vector<std::string>& args);
        void builtinHistory();
        void builtinJobs();
        void builtinFg(const std::vector<std::string>& args);
        void builtinBg(const std::vector<std::string>& args);
        void builtinSource(const std::vector<std::string>& args);

        // Pipeline execution
        void executePipeline(const Pipeline& pipeline);

        // Helper for command substitution $(...) execution
        std::string executeSubshell(const std::string& cmd_str);

        // Output helper
        void print(const std::string& msg);

        std::string cwd_;
        std::string oldpwd_;
        int last_exit_status_ = 0;
        HistoryManager* history_;
        OutputCallback output_cb_;
        JobManager jobs_;
        int in_fd_ = -1;
        int out_fd_ = -1;

        pid_t fg_pgid_ = 0;
        std::atomic<bool> multiwatch_active_{false};
        MultiWatchRunner* current_multiwatch_ = nullptr;
        int cancel_pipe_[2];
    };

} // namespace Terminator

#endif // TERMINATOR_SHELL_HPP

