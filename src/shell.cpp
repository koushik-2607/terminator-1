#include "shell.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <cerrno>

namespace Terminator {

    struct PipeWrapper {
        int fd[2];
    };

    ShellEngine::ShellEngine(HistoryManager* history_mgr, OutputCallback out_cb, int in_fd, int out_fd)
        : history_(history_mgr), output_cb_(out_cb), in_fd_(in_fd), out_fd_(out_fd) {
        
        // Initialize working directory to current process directory
        char buf[PATH_MAX];
        if (getcwd(buf, sizeof(buf))) {
            cwd_ = buf;
        } else {
            cwd_ = "/";
        }
        oldpwd_ = cwd_;

        // Create pipe for interrupting multiWatch or operations via Ctrl+C
        if (pipe(cancel_pipe_) == 0) {
            fcntl(cancel_pipe_[0], F_SETFL, O_NONBLOCK);
            fcntl(cancel_pipe_[1], F_SETFL, O_NONBLOCK);
        } else {
            cancel_pipe_[0] = -1;
            cancel_pipe_[1] = -1;
        }
    }

    ShellEngine::~ShellEngine() {
        jobs_.terminateAll();
        if (cancel_pipe_[0] >= 0) close(cancel_pipe_[0]);
        if (cancel_pipe_[1] >= 0) close(cancel_pipe_[1]);
    }

    void ShellEngine::print(const std::string& msg) {
        if (output_cb_) {
            output_cb_(msg);
        }
    }

    void ShellEngine::pollJobs() {
        std::string notices;
        jobs_.updateStatus(notices);
        if (!notices.empty()) {
            print(notices);
        }
    }

    void ShellEngine::handleCtrlC() {
        // If multiWatch is actively running, signal it to stop
        if (multiwatch_active_ && current_multiwatch_) {
            current_multiwatch_->stop();
            if (cancel_pipe_[1] >= 0) {
                char dummy = 'C';
                ssize_t res = write(cancel_pipe_[1], &dummy, 1);
                (void)res;
            }
        }

        // If a foreground process group is running, signal it with SIGINT
        if (fg_pgid_ > 0) {
            kill(-fg_pgid_, SIGINT);
        }
    }

    void ShellEngine::handleCtrlZ() {
        // If a foreground process group is running, suspend it with SIGTSTP
        if (fg_pgid_ > 0) {
            kill(-fg_pgid_, SIGTSTP);
        }
    }

    std::string ShellEngine::executeSubshell(const std::string& cmd_str) {
        // Execute subshell command (for command substitution) and capture stdout
        int pfd[2];
        if (pipe(pfd) < 0) return "";

        pid_t pid = fork();
        if (pid < 0) {
            close(pfd[0]);
            close(pfd[1]);
            return "";
        }

        if (pid == 0) {
            close(pfd[0]);
            dup2(pfd[1], STDOUT_FILENO);
            close(pfd[1]);

            // Child switches to current tab directory
            if (chdir(cwd_.c_str()) != 0) {
                // Ignore failure
            }

            // Execute subshell command
            execl("/bin/sh", "sh", "-c", cmd_str.c_str(), (char*)nullptr);
            _exit(127);
        }

        close(pfd[1]);
        std::string result;
        char buffer[1024];
        ssize_t n;
        while ((n = read(pfd[0], buffer, sizeof(buffer))) > 0) {
            result.append(buffer, n);
        }
        close(pfd[0]);

        int status;
        waitpid(pid, &status, 0);
        return result;
    }

    void ShellEngine::execute(const std::string& command_line) {
        pollJobs();

        std::string trimmed = trim(command_line);
        if (trimmed.empty()) return;

        // Check if command is multiWatch ["cmd1", "cmd2", ...]
        std::vector<std::string> mw_cmds = Parser::parseMultiWatch(trimmed);
        if (!mw_cmds.empty()) {
            multiwatch_active_ = true;
            // Use in_fd_ if available for Ctrl+C detection, otherwise cancel_pipe_[0]
            int watch_cancel_fd = (in_fd_ >= 0) ? in_fd_ : cancel_pipe_[0];
            MultiWatchRunner mw(mw_cmds, output_cb_, watch_cancel_fd, cwd_);
            current_multiwatch_ = &mw;

            mw.run();

            current_multiwatch_ = nullptr;
            multiwatch_active_ = false;
            last_exit_status_ = 0;
            return;
        }

        // Subshell runner lambda for command substitution $(...)
        auto subshell_runner = [this](const std::string& s) {
            return executeSubshell(s);
        };

        // Parse command pipeline
        Pipeline pipeline = Parser::parsePipeline(trimmed, last_exit_status_, subshell_runner);
        if (pipeline.commands.empty()) return;

        // If pipeline has exactly 1 command with no pipes or background, check for builtins
        if (pipeline.commands.size() == 1 && !pipeline.run_in_background) {
            const auto& cmd = pipeline.commands[0];
            bool is_builtin = false;
            if (!cmd.args.empty()) {
                const std::string& n = cmd.args[0];
                if (n == "cd" || n == "history" || n == "jobs" || 
                    n == "fg" || n == "bg" || n == "source" || 
                    n == "pwd" || n == "exit") {
                    is_builtin = true;
                }
            }

            if (is_builtin) {
                // If builtin has I/O redirection, apply it around the builtin execution
                int saved_stdout = -1;
                int saved_stdin = -1;
                bool redir_failed = false;

                if (!cmd.input_file.empty()) {
                    std::string in_path = expandTilde(cmd.input_file);
                    if (!isAbsolutePath(in_path)) {
                        in_path = (cwd_ == "/") ? ("/" + in_path) : (cwd_ + "/" + in_path);
                    }
                    int in_fd = open(in_path.c_str(), O_RDONLY);
                    if (in_fd < 0) {
                        print("terminator: " + cmd.input_file + ": No such file or directory\n");
                        last_exit_status_ = 1;
                        redir_failed = true;
                    } else {
                        saved_stdin = dup(STDIN_FILENO);
                        dup2(in_fd, STDIN_FILENO);
                        close(in_fd);
                    }
                }

                if (!redir_failed && !cmd.output_file.empty()) {
                    std::string out_path = expandTilde(cmd.output_file);
                    if (!isAbsolutePath(out_path)) {
                        out_path = (cwd_ == "/") ? ("/" + out_path) : (cwd_ + "/" + out_path);
                    }
                    int flags = O_WRONLY | O_CREAT | (cmd.append_output ? O_APPEND : O_TRUNC);
                    int out_fd = open(out_path.c_str(), flags, 0666);
                    if (out_fd < 0) {
                        perror("open output");
                        last_exit_status_ = 1;
                        redir_failed = true;
                    } else {
                        saved_stdout = dup(STDOUT_FILENO);
                        dup2(out_fd, STDOUT_FILENO);
                        close(out_fd);
                    }
                }

                if (!redir_failed) {
                    handleBuiltin(cmd, trimmed);
                }

                // Restore descriptors
                if (saved_stdin >= 0) {
                    dup2(saved_stdin, STDIN_FILENO);
                    close(saved_stdin);
                }
                if (saved_stdout >= 0) {
                    dup2(saved_stdout, STDOUT_FILENO);
                    close(saved_stdout);
                }

                return;
            }
        }

        // Execute external pipeline
        executePipeline(pipeline);
    }

    bool ShellEngine::handleBuiltin(const Command& cmd, const std::string& /*raw_line*/) {
        if (cmd.args.empty()) return false;
        const std::string& name = cmd.args[0];

        if (name == "cd") {
            builtinCd(cmd.args);
            return true;
        }
        if (name == "history") {
            builtinHistory();
            return true;
        }
        if (name == "jobs") {
            builtinJobs();
            return true;
        }
        if (name == "fg") {
            builtinFg(cmd.args);
            return true;
        }
        if (name == "bg") {
            builtinBg(cmd.args);
            return true;
        }
        if (name == "source") {
            builtinSource(cmd.args);
            return true;
        }
        if (name == "pwd") {
            print(cwd_ + "\n");
            last_exit_status_ = 0;
            return true;
        }
        if (name == "exit") {
            // Handled via GUI / tab closer
            print("\033]EXIT\007");
            return true;
        }

        return false;
    }

    void ShellEngine::builtinCd(const std::vector<std::string>& args) {
        std::string target;
        std::string display_arg;

        if (args.size() < 2) {
            const char* home = getenv("HOME");
            target = home ? home : "/";
            display_arg = "~";
        } else if (args[1] == "-") {
            // cd - returns to previous directory (OLDPWD)
            target = oldpwd_;
            display_arg = "-";
            print(oldpwd_ + "\n");
        } else {
            target = expandTilde(args[1]);
            display_arg = args[1];
        }

        // If target is relative, make it relative to tab's cwd
        std::string resolved = target;
        if (!isAbsolutePath(target)) {
            if (cwd_ == "/") {
                resolved = "/" + target;
            } else {
                resolved = cwd_ + "/" + target;
            }
        }

        // Check path resolution with realpath
        char actual_path[PATH_MAX];
        if (realpath(resolved.c_str(), actual_path) != nullptr) {
            struct stat st;
            if (stat(actual_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                oldpwd_ = cwd_;
                cwd_ = actual_path;
                last_exit_status_ = 0;
                // Emit OSC title sequence to update GUI tab header
                print(OSC_TITLE_PREFIX + shortenPath(cwd_) + OSC_TITLE_SUFFIX);
                return;
            }
        }

        // Error report
        print("terminator: cd: " + display_arg + ": No such file or directory\n");
        last_exit_status_ = 1;
    }

    void ShellEngine::builtinHistory() {
        if (!history_) return;
        std::vector<std::string> entries = history_->getAll();
        std::ostringstream oss;
        for (size_t i = 0; i < entries.size(); ++i) {
            oss << std::setw(5) << (i + 1) << "  " << entries[i] << "\n";
        }
        print(oss.str());
        last_exit_status_ = 0;
    }

    void ShellEngine::builtinJobs() {
        pollJobs();
        print(jobs_.listJobs());
        last_exit_status_ = 0;
    }

    void ShellEngine::builtinFg(const std::vector<std::string>& args) {
        std::string spec = (args.size() > 1) ? args[1] : "%+";
        Job* job = jobs_.findJobBySpec(spec);
        if (!job) {
            print("terminator: fg: " + spec + ": no such job\n");
            last_exit_status_ = 1;
            return;
        }

        print(job->command + "\n");
        pid_t pgid = job->pgid;
        std::string cmd_str = job->command;
        int job_id = job->id;

        // Send SIGCONT to the process group in case it was suspended
        kill(-pgid, SIGCONT);
        fg_pgid_ = pgid;

        // Wait for foreground process group
        int status = 0;
        bool stopped = false;
        pid_t wpid;

        while ((wpid = waitpid(-pgid, &status, WUNTRACED)) > 0) {
            if (WIFSTOPPED(status)) {
                stopped = true;
                break;
            }
        }

        if (wpid < 0 && errno == EINTR) {
            // Signal arrived; check child status non-blockingly
            int chk_status;
            pid_t cpid = waitpid(-pgid, &chk_status, WUNTRACED | WNOHANG);
            if (cpid > 0) {
                status = chk_status;
                if (WIFSTOPPED(status)) stopped = true;
            }
        }

        fg_pgid_ = 0;

        if (stopped) {
            job->state = JobState::STOPPED;
            std::ostringstream oss;
            oss << "[" << job_id << "]+ Stopped " << cmd_str << "\n";
            print(oss.str());
            last_exit_status_ = 148; // 128 + SIGTSTP
        } else {
            jobs_.removeJob(job_id);
            last_exit_status_ = WIFEXITED(status) ? WEXITSTATUS(status) : 130;
        }
    }

    void ShellEngine::builtinBg(const std::vector<std::string>& args) {
        std::string spec = (args.size() > 1) ? args[1] : "%+";
        Job* job = jobs_.findJobBySpec(spec);
        if (!job) {
            print("terminator: bg: " + spec + ": no such job\n");
            last_exit_status_ = 1;
            return;
        }

        // Resume process group in background
        kill(-job->pgid, SIGCONT);
        job->state = JobState::RUNNING;

        std::ostringstream oss;
        oss << "[" << job->id << "]+ " << job->command << " &\n";
        print(oss.str());
        last_exit_status_ = 0;
    }

    void ShellEngine::builtinSource(const std::vector<std::string>& args) {
        if (args.size() < 2) {
            print("terminator: source: filename argument required\n");
            last_exit_status_ = 1;
            return;
        }

        std::string script_path = expandTilde(args[1]);
        if (!isAbsolutePath(script_path)) {
            script_path = (cwd_ == "/") ? ("/" + script_path) : (cwd_ + "/" + script_path);
        }

        std::ifstream file(script_path);
        if (!file.is_open()) {
            print("terminator: source: " + args[1] + ": No such file or directory\n");
            last_exit_status_ = 1;
            return;
        }

        std::string line;
        std::string accumulated;

        while (std::getline(file, line)) {
            std::string t = trim(line);
            if (accumulated.empty() && (t.empty() || t[0] == '#')) continue;

            if (accumulated.empty()) {
                accumulated = line;
            } else {
                accumulated += "\n" + line;
            }

            if (!Parser::isInputIncomplete(accumulated)) {
                execute(accumulated);
                accumulated.clear();
            }
        }

        if (!accumulated.empty()) {
            execute(accumulated);
        }

        last_exit_status_ = 0;
    }

    void ShellEngine::executePipeline(const Pipeline& pipeline) {
        size_t num_cmds = pipeline.commands.size();
        if (num_cmds == 0) return;

        // Check if any input redirection target is missing before running any child
        for (const auto& cmd : pipeline.commands) {
            if (!cmd.input_file.empty()) {
                std::string in_target = expandTilde(cmd.input_file);
                if (!isAbsolutePath(in_target)) {
                    in_target = (cwd_ == "/") ? ("/" + in_target) : (cwd_ + "/" + in_target);
                }
                struct stat st;
                if (stat(in_target.c_str(), &st) != 0) {
                    print("terminator: " + cmd.input_file + ": No such file or directory\n");
                    last_exit_status_ = 1;
                    return;
                }
            }
        }

        // Create N-1 pipes for N commands
        std::vector<PipeWrapper> pipes(num_cmds > 1 ? num_cmds - 1 : 0);
        for (size_t i = 0; i < pipes.size(); ++i) {
            if (pipe(pipes[i].fd) < 0) {
                perror("pipe");
                last_exit_status_ = 1;
                return;
            }
        }

        std::vector<pid_t> pids(num_cmds, -1);
        pid_t pgid = 0;

        for (size_t i = 0; i < num_cmds; ++i) {
            const auto& cmd = pipeline.commands[i];

            pid_t pid = fork();
            if (pid < 0) {
                perror("fork");
                return;
            }

            if (pid == 0) {
                // Child process: set process group ID
                if (i == 0) {
                    setpgid(0, 0);
                } else {
                    setpgid(0, pgid);
                }

                // Change directory to tab's cwd
                if (chdir(cwd_.c_str()) != 0) {
                    // Ignore
                }

                // Connect pipe input
                if (i > 0) {
                    dup2(pipes[i - 1].fd[0], STDIN_FILENO);
                }

                // Connect pipe output
                if (i < num_cmds - 1) {
                    dup2(pipes[i].fd[1], STDOUT_FILENO);
                } else if (cmd.output_file.empty() && out_fd_ >= 0) {
                    dup2(out_fd_, STDOUT_FILENO);
                    dup2(out_fd_, STDERR_FILENO);
                }

                // Close all pipe descriptors in child
                for (size_t j = 0; j < pipes.size(); ++j) {
                    close(pipes[j].fd[0]);
                    close(pipes[j].fd[1]);
                }

                // Handle file input redirection '<'
                if (!cmd.input_file.empty()) {
                    std::string in_path = expandTilde(cmd.input_file);
                    if (!isAbsolutePath(in_path)) {
                        in_path = (cwd_ == "/") ? ("/" + in_path) : (cwd_ + "/" + in_path);
                    }
                    int in_fd = open(in_path.c_str(), O_RDONLY);
                    if (in_fd < 0) {
                        std::cerr << "terminator: " << cmd.input_file << ": No such file or directory\n";
                        _exit(1);
                    }
                    dup2(in_fd, STDIN_FILENO);
                    close(in_fd);
                }

                // Handle file output redirection '>' or '>>'
                if (!cmd.output_file.empty()) {
                    std::string out_path = expandTilde(cmd.output_file);
                    if (!isAbsolutePath(out_path)) {
                        out_path = (cwd_ == "/") ? ("/" + out_path) : (cwd_ + "/" + out_path);
                    }
                    int flags = O_WRONLY | O_CREAT | (cmd.append_output ? O_APPEND : O_TRUNC);
                    int out_fd = open(out_path.c_str(), flags, 0666);
                    if (out_fd < 0) {
                        perror("open output");
                        _exit(1);
                    }
                    dup2(out_fd, STDOUT_FILENO);
                    close(out_fd);
                }

                // Prepare argv for execvp
                if (cmd.args.empty()) {
                    _exit(0);
                }

                std::vector<char*> argv;
                for (const auto& arg : cmd.args) {
                    argv.push_back(const_cast<char*>(arg.c_str()));
                }
                argv.push_back(nullptr);

                execvp(argv[0], argv.data());

                // If execvp returns, command was not found or failed
                if (errno == ENOENT) {
                    std::cerr << "terminator: " << cmd.args[0] << ": command not found\n";
                    _exit(127);
                } else {
                    std::cerr << "terminator: " << cmd.args[0] << ": " << strerror(errno) << "\n";
                    _exit(126);
                }
            }

            // Parent setup
            pids[i] = pid;
            if (i == 0) {
                pgid = pid;
            }
            setpgid(pid, pgid);
        }

        // Close all pipe descriptors in parent
        for (size_t i = 0; i < pipes.size(); ++i) {
            close(pipes[i].fd[0]);
            close(pipes[i].fd[1]);
        }

        if (pipeline.run_in_background) {
            // Background job
            int job_id = jobs_.addJob(pgid, pids, pipeline.raw_command, JobState::RUNNING);
            std::ostringstream oss;
            oss << "[" << job_id << "] " << pgid << "\n";
            print(oss.str());
            last_exit_status_ = 0;
        } else {
            // Foreground job
            fg_pgid_ = pgid;
            bool any_stopped = false;
            int last_status = 0;

            for (pid_t pid : pids) {
                int status = 0;
                pid_t res = waitpid(pid, &status, WUNTRACED);
                if (res > 0) {
                    if (WIFSTOPPED(status)) {
                        any_stopped = true;
                    }
                    last_status = status;
                } else if (res < 0 && errno == EINTR) {
                    // Check if child was stopped or exited
                    int chk = 0;
                    if (waitpid(pid, &chk, WUNTRACED | WNOHANG) > 0) {
                        if (WIFSTOPPED(chk)) any_stopped = true;
                        last_status = chk;
                    }
                }
            }

            fg_pgid_ = 0;

            if (any_stopped) {
                int job_id = jobs_.addJob(pgid, pids, pipeline.raw_command, JobState::STOPPED);
                std::ostringstream oss;
                oss << "\n[" << job_id << "]+ Stopped " << pipeline.raw_command << "\n";
                print(oss.str());
                last_exit_status_ = 148;
            } else {
                if (WIFEXITED(last_status)) {
                    last_exit_status_ = WEXITSTATUS(last_status);
                } else if (WIFSIGNALED(last_status)) {
                    last_exit_status_ = 128 + WTERMSIG(last_status);
                }
            }
        }
    }

} // namespace Terminator
