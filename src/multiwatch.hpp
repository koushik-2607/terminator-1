#ifndef TERMINATOR_MULTIWATCH_HPP // Prevent duplicated multiWatch declarations when included in multiple modules.
#define TERMINATOR_MULTIWATCH_HPP // Define the header guard symbol.

#include "common.hpp" // Provides pid_t, poll, and POSIX descriptor/process declarations.
#include <string> // Stores commands and accumulated output.
#include <vector> // Owns one state record for every monitored command.
#include <atomic> // Allows a stop request to be observed safely across execution contexts.
#include <functional> // Defines the callback type used to send text to the tab UI.

namespace Terminator {

    /**
     * Represents a single watched command in multiWatch.
     */
    struct MonitoredCommand { // State for one independently launched command in a multiWatch list.
        std::string command; // Original shell command sent to /bin/sh -c.
        pid_t pid = -1; // Child PID, or -1 when no child is currently active.
        int pipe_read_fd = -1; // Parent's read end for this child's captured output pipe.
        std::string output_buffer; // Partial output retained while lines arrive in chunks.
        time_t finished_at = 0; // Wall-clock completion time used to schedule the next run.
        bool is_running = false; // Distinguishes an active child from a command waiting for its next interval.
    };

    /**
     * MultiWatchRunner manages parallel command monitoring.
     * Implements non-busy-wait polling using poll() on all command pipes.
     * Automatically restarts finished commands every 2 seconds.
     * Safely terminates all child processes when Ctrl+C is signaled.
     */
    class MultiWatchRunner { // Runs a set of commands concurrently and relays their output until cancellation.
    public:
        using OutputCallback = std::function<void(const std::string&)>; // Callable that forwards formatted output to the shell/tab.

        MultiWatchRunner(const std::vector<std::string>& commands, // Commands to launch in parallel and periodically restart.
                         OutputCallback out_cb,
                         int cancel_pipe_fd = -1,
                         const std::string& cwd = "");

        ~MultiWatchRunner(); // Ensure child processes and pipe descriptors are cleaned up if run exits early.

        /**
         * Run the multiWatch event loop. Blocks until stopped by Ctrl+C.
         */
        void run(); // Poll child output/cancel descriptors and restart completed commands until stopped.

        /**
         * Request immediate termination of all child processes and exit loop.
         */
        void stop(); // Atomically request loop termination and signal active children.

    private:
        void startCommand(MonitoredCommand& mc); // Fork one command and connect its stdout/stderr to a pipe.
        void printPiece(MonitoredCommand& mc, const std::string& text); // Add the required timestamp/divider formatting for output.
        void reapAndFinish(MonitoredCommand& mc); // Wait nonblockingly and record the child's completion time.
        void terminateAll(); // Stop all active process groups and close their pipe descriptors.

        std::vector<MonitoredCommand> commands_; // Owns lifecycle state and pipe information for each watched command.
        OutputCallback output_cb_; // Sends generated strings back through the shell output channel.
        int cancel_fd_; // Optional descriptor watched for Ctrl+C/cancellation notifications; -1 means none.
        std::string cwd_; // Directory applied to each child before it executes the command.
        std::atomic<bool> running_{false}; // Shared stop flag checked by the polling loop.
    };

} // namespace Terminator

#endif // TERMINATOR_MULTIWATCH_HPP closes the include guard.

// EOF
