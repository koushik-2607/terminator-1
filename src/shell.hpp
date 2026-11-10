#ifndef TERMINATOR_SHELL_HPP // Avoid duplicate ShellEngine declarations when included from GUI, tab, and test files.
#define TERMINATOR_SHELL_HPP // Mark the shell interface as already included.

#include "common.hpp" // Supplies process IDs, descriptors, signals, and common helpers.
#include "parser.hpp" // Provides parsed Command/Pipeline types and local-variable expansion.
#include "jobs.hpp" // Gives this shell a per-tab process-group job table.
#include "history.hpp" // Allows the history built-in to read the shared command list.
#include "multiwatch.hpp" // Provides parallel command-monitoring support.
#include <string> // Represents commands, paths, and shell text.
#include <vector> // Holds parsed arguments and job-related data.
#include <functional> // Defines OutputCallback, a callable UI/pipe output adapter.
#include <atomic> // Makes the multiWatch-active flag safe to observe across control paths.
#include <unordered_map> // Stores variables per ShellEngine rather than leaking them across thread-mode tabs.

namespace Terminator {

    /**
     * ShellEngine executes commands, manages current directory, job control,
     * environment variables, and builtins for a single tab.
     */
    class ShellEngine { // Implements one tab's shell state and command execution behavior.
    public:
        using OutputCallback = std::function<void(const std::string&)>; // Lets output route to a GUI buffer or an IPC pipe without coupling the shell to X11.

        ShellEngine(HistoryManager* history_mgr, OutputCallback out_cb, int in_fd = -1, int out_fd = -1); // Bind shared history, output, and optional control/output descriptors.
        ~ShellEngine(); // Stop jobs and close internal descriptors before the tab worker exits.

        /**
         * Execute a complete command line.
         * Handles builtins, pipelines, redirections, background jobs, and multiWatch.
         */
        void execute(const std::string& command_line); // Parse and run one complete command line, selecting built-ins or child processes.

        /**
         * Handle Ctrl+C signal from GUI for this tab.
         */
        void handleCtrlC(); // Forward SIGINT to this shell's foreground group and stop multiWatch if active.

        /**
         * Handle Ctrl+Z signal from GUI for this tab.
         */
        void handleCtrlZ(); // Forward SIGTSTP to this shell's foreground process group.

        /**
         * Poll job statuses and output any completion notifications.
         */
        void pollJobs(); // Nonblockingly collect child status changes and print job notifications.

        /**
         * Current working directory of this tab.
         */
        std::string getCwd() const { return cwd_; } // Return the tab-local current directory.

        /**
         * Tab title (shortened path: e.g. ~/Downloads).
         */
        std::string getTitle() const { return shortenPath(cwd_); } // Return a compact HOME-relative label for the GUI tab bar.

        /**
         * Check if a foreground process is currently running.
         */
        bool isForegroundRunning() const { return fg_pgid_ > 0; } // A positive process-group ID means a foreground child group is active.

        /**
         * Last command exit status ($?).
         */
        int getLastExitStatus() const { return last_exit_status_; } // Expose the value expanded by the parser for $? .

        /**
         * Job manager reference.
         */
        JobManager& getJobManager() { return jobs_; } // Return this tab's mutable job manager for shell/GUI coordination.

    private:
        // Built-in command handlers
        bool handleBuiltin(const Command& cmd, const std::string& raw_line); // Run an internal command in the shell process and report whether it matched.
        void builtinCd(const std::vector<std::string>& args); // Update this tab's cwd without changing other tabs' directories.
        void builtinHistory(); // Format and print the recent entries from shared history.
        void builtinJobs(); // Refresh job states and print the current job table.
        void builtinFg(const std::vector<std::string>& args); // Resume a selected job in foreground and wait for it.
        void builtinBg(const std::vector<std::string>& args); // Resume a stopped job without waiting in foreground.
        void builtinSource(const std::vector<std::string>& args); // Interpret a script using this shell's variables and command executor.

        // Pipeline execution
        void executePipeline(const Pipeline& pipeline); // Fork pipeline stages, connect descriptors, and wait or register a background job.

        // Helper for command substitution $(...) execution
        std::string executeSubshell(const std::string& cmd_str); // Run substitution text in a child and capture its standard output.

        // Output helper
        void print(const std::string& msg); // Send shell output through the callback selected by the owning tab.

        std::string cwd_; // Current directory tracked independently for this shell/tab.
        std::string oldpwd_; // Previous directory used by cd -.
        int last_exit_status_ = 0; // Exit code of the last command, exposed to scripts and $? expansion.
        HistoryManager* history_; // Non-owning pointer to GUI-owned shared history; its lifetime outlasts tabs.
        OutputCallback output_cb_; // Function that sends text to the appropriate terminal output stream.
        JobManager jobs_; // Owns background/stopped jobs belonging only to this tab.
        int in_fd_ = -1; // Optional shell-control input descriptor used to notice Ctrl+C while waiting.
        int out_fd_ = -1; // Optional descriptor child processes use for captured standard output/error.

        pid_t fg_pgid_ = 0; // Process-group ID currently receiving foreground signals; zero means none.
        std::atomic<bool> multiwatch_active_{false}; // Indicates whether Ctrl+C should cancel the multiWatch loop.
        MultiWatchRunner* current_multiwatch_ = nullptr; // Non-owning pointer valid while execute() is inside multiWatch.
        int cancel_pipe_[2]; // Internal nonblocking pipe used to wake cancellation-aware waits.
        Parser::VariableMap variables_; // Tab-local script variables and positional parameters, isolated across thread-mode shells.
    };

} // namespace Terminator

#endif // TERMINATOR_SHELL_HPP closes the include guard.

// EOF
