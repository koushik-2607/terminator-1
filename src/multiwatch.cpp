#include "multiwatch.hpp"
#include <ctime>
#include <iostream>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/time.h>

namespace Terminator {

    MultiWatchRunner::MultiWatchRunner(const std::vector<std::string>& commands, 
                                       OutputCallback out_cb,
                                       int cancel_pipe_fd,
                                       const std::string& cwd)
        : output_cb_(out_cb), cancel_fd_(cancel_pipe_fd), cwd_(cwd) {
        
        for (const auto& cmd : commands) {
            MonitoredCommand mc;
            mc.command = cmd;
            commands_.push_back(mc);
        }
    }

    MultiWatchRunner::~MultiWatchRunner() {
        terminateAll();
    }

    void MultiWatchRunner::stop() {
        running_ = false;
        terminateAll();
    }

    void MultiWatchRunner::terminateAll() {
        for (auto& mc : commands_) {
            if (mc.is_running && mc.pid > 0) {
                // Terminate child process and process group
                kill(-mc.pid, SIGTERM);
                kill(mc.pid, SIGTERM);
                int status;
                waitpid(mc.pid, &status, WNOHANG);
                mc.is_running = false;
            }
            if (mc.pipe_read_fd >= 0) {
                close(mc.pipe_read_fd);
                mc.pipe_read_fd = -1;
            }
        }
    }

    void MultiWatchRunner::startCommand(MonitoredCommand& mc) {
        int pipefd[2];
        if (pipe(pipefd) < 0) {
            perror("multiWatch pipe");
            return;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("multiWatch fork");
            close(pipefd[0]);
            close(pipefd[1]);
            return;
        }

        if (pid == 0) {
            // Child process: set its own process group
            setpgid(0, 0);

            // Change to current working directory of the tab
            if (!cwd_.empty()) {
                if (chdir(cwd_.c_str()) != 0) {
                    // Ignore failure
                }
            }

            // Redirect stdout and stderr to write end of pipe
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);

            // Execute command via /bin/sh -c
            execl("/bin/sh", "sh", "-c", mc.command.c_str(), (char*)nullptr);
            _exit(127);
        }

        // Parent process
        close(pipefd[1]);
        mc.pid = pid;
        mc.pipe_read_fd = pipefd[0];
        mc.is_running = true;
        mc.output_buffer.clear();
    }

    void MultiWatchRunner::printPiece(MonitoredCommand& mc, const std::string& text) {
        if (text.empty()) return;

        // Required exact format (52 dashes):
        // "cmd1", <unix_timestamp>:
        // ----------------------------------------------------
        // <output of cmd1>
        // ----------------------------------------------------
        time_t now = time(nullptr);
        std::ostringstream oss;
        oss << "\"" << mc.command << "\", " << now << ":\n";
        oss << "----------------------------------------------------\n";
        oss << text;
        if (text.back() != '\n') {
            oss << "\n";
        }
        oss << "----------------------------------------------------\n";

        if (output_cb_) {
            output_cb_(oss.str());
        }
    }

    void MultiWatchRunner::reapAndFinish(MonitoredCommand& mc) {
        if (!mc.output_buffer.empty()) {
            printPiece(mc, mc.output_buffer);
            mc.output_buffer.clear();
        }

        if (mc.pipe_read_fd >= 0) {
            close(mc.pipe_read_fd);
            mc.pipe_read_fd = -1;
        }

        mc.is_running = false;
        mc.finished_at = time(nullptr);

        if (mc.pid > 0) {
            int status;
            waitpid(mc.pid, &status, WNOHANG);
            mc.pid = -1;
        }
    }

    void MultiWatchRunner::run() {
        running_ = true;

        // Launch all monitored commands in parallel
        for (auto& mc : commands_) {
            startCommand(mc);
        }

        while (running_) {
            // Check if any finished command is ready for 2-second restart
            time_t now = time(nullptr);
            for (auto& mc : commands_) {
                if (!mc.is_running && mc.finished_at > 0 && (now - mc.finished_at >= MULTIWATCH_INTERVAL_SEC)) {
                    startCommand(mc);
                }
            }

            // Build pollfd set for read pipes and cancel fd
            std::vector<struct pollfd> pfds;
            std::vector<MonitoredCommand*> pfd_to_cmd;

            if (cancel_fd_ >= 0) {
                struct pollfd p;
                p.fd = cancel_fd_;
                p.events = POLLIN | POLLHUP;
                p.revents = 0;
                pfds.push_back(p);
                pfd_to_cmd.push_back(nullptr);
            }

            for (auto& mc : commands_) {
                if (mc.is_running && mc.pipe_read_fd >= 0) {
                    struct pollfd p;
                    p.fd = mc.pipe_read_fd;
                    p.events = POLLIN | POLLHUP;
                    p.revents = 0;
                    pfds.push_back(p);
                    pfd_to_cmd.push_back(&mc);
                }
            }

            // Poll with 100ms timeout to avoid busy-waiting while remaining responsive
            int ret = poll(pfds.data(), pfds.size(), 100);

            if (!running_) break;

            if (ret > 0) {
                for (size_t i = 0; i < pfds.size(); ++i) {
                    if (pfds[i].fd == cancel_fd_ && (pfds[i].revents & (POLLIN | POLLHUP))) {
                        char cancel_buf[64];
                        ssize_t cn = read(cancel_fd_, cancel_buf, sizeof(cancel_buf));
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

                    MonitoredCommand* mc = pfd_to_cmd[i];
                    if (!mc) continue;

                    if (pfds[i].revents & POLLIN) {
                        char buf[1024];
                        ssize_t bytes = read(mc->pipe_read_fd, buf, sizeof(buf) - 1);
                        if (bytes > 0) {
                            buf[bytes] = '\0';
                            mc->output_buffer += buf;
                            // If output contains newline, print it immediately as it arrives
                            size_t last_nl = mc->output_buffer.rfind('\n');
                            if (last_nl != std::string::npos) {
                                std::string to_print = mc->output_buffer.substr(0, last_nl + 1);
                                printPiece(*mc, to_print);
                                mc->output_buffer = mc->output_buffer.substr(last_nl + 1);
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
                    int status;
                    pid_t res = waitpid(mc.pid, &status, WNOHANG);
                    if (res > 0) {
                        // Read whatever remaining bytes in pipe
                        if (mc.pipe_read_fd >= 0) {
                            char buf[1024];
                            ssize_t bytes;
                            while ((bytes = read(mc.pipe_read_fd, buf, sizeof(buf) - 1)) > 0) {
                                buf[bytes] = '\0';
                                mc.output_buffer += buf;
                            }
                        }
                        reapAndFinish(mc);
                    }
                }
            }
        }

        terminateAll();
    }

} // namespace Terminator
