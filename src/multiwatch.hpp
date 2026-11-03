#ifndef TERMINATOR_MULTIWATCH_HPP
#define TERMINATOR_MULTIWATCH_HPP

#include "common.hpp"
#include <string>
#include <vector>
#include <atomic>
#include <functional>

namespace Terminator {

    /**
     * Represents a single watched command in multiWatch.
     */
    struct MonitoredCommand {
        std::string command;
        pid_t pid = -1;
        int pipe_read_fd = -1;
        std::string output_buffer;
        time_t finished_at = 0;
        bool is_running = false;
    };

    /**
     * MultiWatchRunner manages parallel command monitoring.
     * Implements non-busy-wait polling using poll() on all command pipes.
     * Automatically restarts finished commands every 2 seconds.
     * Safely terminates all child processes when Ctrl+C is signaled.
     */
    class MultiWatchRunner {
    public:
        using OutputCallback = std::function<void(const std::string&)>;

        MultiWatchRunner(const std::vector<std::string>& commands, 
                         OutputCallback out_cb,
                         int cancel_pipe_fd = -1,
                         const std::string& cwd = "");

        ~MultiWatchRunner();

        /**
         * Run the multiWatch event loop. Blocks until stopped by Ctrl+C.
         */
        void run();

        /**
         * Request immediate termination of all child processes and exit loop.
         */
        void stop();

    private:
        void startCommand(MonitoredCommand& mc);
        void printPiece(MonitoredCommand& mc, const std::string& text);
        void reapAndFinish(MonitoredCommand& mc);
        void terminateAll();

        std::vector<MonitoredCommand> commands_;
        OutputCallback output_cb_;
        int cancel_fd_;
        std::string cwd_;
        std::atomic<bool> running_{false};
    };

} // namespace Terminator

#endif // TERMINATOR_MULTIWATCH_HPP

