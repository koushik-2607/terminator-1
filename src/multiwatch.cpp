#include "multiwatch.hpp" // Declares monitored-command state and the MultiWatchRunner interface.
#include <ctime> // Provides time() for timestamps and restart scheduling.
#include <iostream> // Provides standard error streams for diagnostics.
#include <unistd.h> // Provides pipe, fork, close, dup2, chdir, read, and _exit.
#include <sys/wait.h> // Provides waitpid and its nonblocking wait options.
#include <sys/time.h> // POSIX time declarations used by platform code.

namespace Terminator {

    MultiWatchRunner::MultiWatchRunner(const std::vector<std::string>& commands, // Receive command texts by reference to avoid copying the input argument list.
                                       OutputCallback out_cb,
                                       int cancel_pipe_fd,
                                       const std::string& cwd)
        : output_cb_(out_cb), cancel_fd_(cancel_pipe_fd), cwd_(cwd) { // Copy callback/control fd/directory into this runner's owned state.
        
        for (const auto& cmd : commands) { // Create one lifecycle record for each requested command.
            MonitoredCommand mc; // Default member initializers mark it as not yet running.
            mc.command = cmd; // Preserve the exact command string used for execution and formatted output.
            commands_.push_back(mc); // Store the record so run() can start and monitor it.
        }
    }

    MultiWatchRunner::~MultiWatchRunner() { // RAII cleanup also runs when run exits due to an error or exception.
        terminateAll(); // Signal children and close pipe descriptors before destroying command records.
    }

    void MultiWatchRunner::stop() {
        running_ = false; // Tell the polling loop to stop at its next check.
        terminateAll(); // Immediately stop child groups instead of waiting for a poll timeout.
    }

    void MultiWatchRunner::terminateAll() {
        for (auto& mc : commands_) { // Cleanup every command independently so one invalid child does not skip others.
            if (mc.is_running && mc.pid > 0) { // Only signal records with a real active child.
                // Terminate child process and process group
                kill(-mc.pid, SIGTERM); // Negative PID sends SIGTERM to the child's process group.
                kill(mc.pid, SIGTERM); // Also target the PID directly as a fallback if group setup failed.
                int status; // waitpid writes encoded child status here when it has already exited.
                waitpid(mc.pid, &status, WNOHANG); // Reap only if immediately available; cleanup must not hang.
                mc.is_running = false; // Prevent later polling from treating this child as live.
            }
            if (mc.pipe_read_fd >= 0) { // Valid descriptors are nonnegative; -1 means already closed/uninitialized.
                close(mc.pipe_read_fd); // Release the parent's read endpoint.
                mc.pipe_read_fd = -1; // Mark it closed to make cleanup safe to call more than once.
            }
        }
    }

    void MultiWatchRunner::startCommand(MonitoredCommand& mc) {
        int pipefd[2]; // pipe() fills [0] with the read end and [1] with the write end.
        if (pipe(pipefd) < 0) { // A negative return indicates descriptor allocation failed.
            perror("multiWatch pipe");
            return;
        }

        pid_t pid = fork(); // Duplicate the process; child receives zero and parent receives its child's PID.
        if (pid < 0) { // Negative means no child was created.
            perror("multiWatch fork");
            close(pipefd[0]); // Parent cannot use this pipe after a failed fork.
            close(pipefd[1]); // Close both ends to avoid descriptor leaks.
            return; // Leave the command not running so run() can retry later.
        }

        if (pid == 0) {
            // Child process: set its own process group
            setpgid(0, 0); // Make this child a process-group leader so signals target the whole command group.

            // Change to current working directory of the tab
            if (!cwd_.empty()) { // Empty cwd means keep the inherited working directory.
                if (chdir(cwd_.c_str()) != 0) { // Switch only the child; parent and other tabs retain their directories.
                    // Ignore failure
                }
            }

            // Redirect stdout and stderr to write end of pipe
            close(pipefd[0]); // Child writes output, so it closes the unused read endpoint.
            dup2(pipefd[1], STDOUT_FILENO); // Replace standard output with the pipe writer.
            dup2(pipefd[1], STDERR_FILENO); // Capture standard error through the same pipe as standard output.
            close(pipefd[1]); // The duplicated standard descriptors now keep the pipe open.

            // Execute command via /bin/sh -c
            execl("/bin/sh", "sh", "-c", mc.command.c_str(), (char*)nullptr); // Replace this child with a shell interpreting the monitor command.
            _exit(127); // Reached only when exec fails; use _exit to avoid running inherited C++ cleanup handlers.
        }

        // Parent process
        close(pipefd[1]); // Parent only reads from this pipe and must close its unused writer for EOF to arrive.
        mc.pid = pid; // Save child PID for waitpid and process-group signaling.
        mc.pipe_read_fd = pipefd[0]; // Retain the pipe reader for poll/read in the event loop.
        mc.is_running = true; // Include this command in the live poll set.
        mc.output_buffer.clear(); // Start a fresh buffer for this run's partial output.
    }

    void MultiWatchRunner::printPiece(MonitoredCommand& mc, const std::string& text) {
        if (text.empty()) return; // Avoid printing empty blocks when a child emits no output.

        // Required exact format (52 dashes):
        // "cmd1", <unix_timestamp>:
        // ----------------------------------------------------
        // <output of cmd1>
        // ----------------------------------------------------
        time_t now = time(nullptr); // Unix seconds timestamp the output block for assignment-compatible formatting.
        std::ostringstream oss; // Assemble one complete block before forwarding it to the GUI.
        oss << "\"" << mc.command << "\", " << now << ":\n";
        oss << "----------------------------------------------------\n";
        oss << text;
        if (text.back() != '\n') { // Ensure the payload ends above the closing divider.
            oss << "\n"; // Add a line break only when the command did not provide one.
        }
        oss << "----------------------------------------------------\n";

        if (output_cb_) { // The callback may be empty in a caller that only wants process control.
            output_cb_(oss.str()); // Forward the formatted block into the tab's output channel.
        }
    }

    void MultiWatchRunner::reapAndFinish(MonitoredCommand& mc) {
        if (!mc.output_buffer.empty()) { // Flush a final partial line that had no trailing newline.
            printPiece(mc, mc.output_buffer); // Preserve that last output before closing the pipe.
            mc.output_buffer.clear(); // Avoid printing the same bytes if cleanup is invoked again.
        }

        if (mc.pipe_read_fd >= 0) { // Close the descriptor once EOF/exit has been observed.
            close(mc.pipe_read_fd); // Release the read end in the parent process.
            mc.pipe_read_fd = -1; // Mark as unavailable for future poll construction.
        }

        mc.is_running = false; // The scheduler may now wait for the restart interval.
        mc.finished_at = time(nullptr); // Record the time used to decide when to relaunch this command.

        if (mc.pid > 0) { // A positive PID may still need a nonblocking reap after pipe EOF.
            int status; // waitpid output storage; the status is intentionally not used here.
            waitpid(mc.pid, &status, WNOHANG); // Reap if ready without blocking the multiWatch loop.
            mc.pid = -1; // Forget the finished child so it cannot be signaled again.
        }
    }

    void MultiWatchRunner::run() { // Start all commands and coordinate output, cancellation, and timed restarts.
        running_ = true; // Reset the atomic stop flag at the beginning of each run invocation.

        // Launch all monitored commands in parallel
        for (auto& mc : commands_) { // Start every command before waiting so they run concurrently.
            startCommand(mc); // Each child has an independent output pipe and process group.
        }

        while (running_) { // Rebuild the poll set each time to reflect current children and pipe descriptors.
            // Check if any finished command is ready for 2-second restart
            time_t now = time(nullptr); // Snapshot current wall-clock time for all restart decisions in this iteration.
            for (auto& mc : commands_) { // Revisit finished commands to maintain their periodic schedule.
                if (!mc.is_running && mc.finished_at > 0 && (now - mc.finished_at >= MULTIWATCH_INTERVAL_SEC)) { // Ensure a prior run finished and the interval elapsed.
                    startCommand(mc); // Relaunch only this command; the others continue independently.
                }
            }

            // Build pollfd set for read pipes and cancel fd
            std::vector<struct pollfd> pfds; // poll() input/output array for all active read and cancel descriptors.
            std::vector<MonitoredCommand*> pfd_to_cmd; // Parallel mapping from poll array position to owning command.

            if (cancel_fd_ >= 0) { // Include cancellation only when the caller supplied a valid descriptor.
                struct pollfd p; // One descriptor registration consumed by poll().
                p.fd = cancel_fd_; // Watch the GUI-to-shell control pipe.
                p.events = POLLIN | POLLHUP; // Wake for readable cancellation bytes or closure by the writer.
                p.revents = 0; // poll fills this output field with observed event bits.
                pfds.push_back(p); // Add it to the descriptor array.
                pfd_to_cmd.push_back(nullptr); // Null marks this slot as the cancel descriptor, not command output.
            }

            for (auto& mc : commands_) {
                if (mc.is_running && mc.pipe_read_fd >= 0) { // Only active commands with open readers belong in poll().
                    struct pollfd p;
                    p.fd = mc.pipe_read_fd; // poll this command's pipe for output and EOF.
                    p.events = POLLIN | POLLHUP; // Readable bytes or hangup both require handling.
                    p.revents = 0; // Initialize the output event mask before passing it to poll.
                    pfds.push_back(p); // Add this descriptor to the poll set.
                    pfd_to_cmd.push_back(&mc); // Save the matching command address at the same index.
                }
            }

            // Poll with 100ms timeout to avoid busy-waiting while remaining responsive
            int ret = poll(pfds.data(), pfds.size(), 100); // Sleep up to 100 ms, waking early when output or cancellation is ready.

            if (!running_) break; // stop() may set the flag while poll is waiting.

            if (ret > 0) { // At least one descriptor has an event bit in revents.
                for (size_t i = 0; i < pfds.size(); ++i) { // Keep index aligned with pfd_to_cmd's owner pointer.
                    if (pfds[i].fd == cancel_fd_ && (pfds[i].revents & (POLLIN | POLLHUP))) {
                        char cancel_buf[64]; // Small buffer for control bytes written by the GUI.
                        ssize_t cn = read(cancel_fd_, cancel_buf, sizeof(cancel_buf)); // Drain available bytes without blocking.
                        if (cn > 0) {
                            for (ssize_t ci = 0; ci < cn; ++ci) {
                                if (cancel_buf[ci] == '\003') { // Ctrl+C
                                    running_ = false;
                                    break;
                                }
                            }
                        } else if (cn == 0) {
                            running_ = false;
                        }
                        if (!running_) break;
                        continue;
                    }

                    MonitoredCommand* mc = pfd_to_cmd[i]; // Null is reserved for the cancellation descriptor.
                    if (!mc) continue; // Cancellation was already processed by the branch above.

                    if (pfds[i].revents & POLLIN) {
                        char buf[1024]; // Reserve the last byte for NUL termination before string appending.
                        ssize_t bytes = read(mc->pipe_read_fd, buf, sizeof(buf) - 1); // Read bytes currently available from this child.
                        if (bytes > 0) {
                            buf[bytes] = '\0'; // Make the received bytes safe for C-string operations.
                            mc->output_buffer += buf; // Retain fragments until at least one newline is available.
                            // If output contains newline, print it immediately as it arrives
                            size_t last_nl = mc->output_buffer.rfind('\n'); // Locate the final complete line in the buffer.
                            if (last_nl != std::string::npos) {
                                std::string to_print = mc->output_buffer.substr(0, last_nl + 1); // Include the final newline in the printable chunk.
                                printPiece(*mc, to_print); // Forward complete output promptly to the terminal UI.
                                mc->output_buffer = mc->output_buffer.substr(last_nl + 1); // Preserve only the partial line tail.
                            }
                        } else if (bytes == 0) {
                            // Pipe reached EOF
                            reapAndFinish(*mc);
                        }
                    } else if (pfds[i].revents & (POLLHUP | POLLERR)) {
                        reapAndFinish(*mc);
                    }
                }
            }

            // Also check for child process terminations
            for (auto& mc : commands_) {
                if (mc.is_running && mc.pid > 0) {
                    int status; // waitpid writes the child's encoded exit/termination details here.
                    pid_t res = waitpid(mc.pid, &status, WNOHANG); // Reap if complete without blocking this polling loop.
                    if (res > 0) {
                        // Read whatever remaining bytes in pipe
                        if (mc.pipe_read_fd >= 0) {
                            char buf[1024]; // Temporary storage for any final pipe bytes.
                            ssize_t bytes;
                            while ((bytes = read(mc.pipe_read_fd, buf, sizeof(buf) - 1)) > 0) {
                                buf[bytes] = '\0'; // Terminate this final read chunk before appending it.
                                mc.output_buffer += buf; // Keep every byte not yet delivered to the callback.
                            }
                        }
                        reapAndFinish(mc); // Flush remaining text and record the finish time for scheduled restart.
                    }
                }
            }
        }

        terminateAll(); // Ensure all remaining children and pipe descriptors are cleaned up on loop exit.
    }

} // namespace Terminator
// EOF
