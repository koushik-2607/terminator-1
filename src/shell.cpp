#include "shell.hpp" // Declares per-tab shell state and the command/parser interfaces.
#include <fstream> // Reads script files for the internal interpreter.
#include <sstream> // Formats history, job, and error output.
#include <iomanip> // Applies aligned numeric formatting to history listings.
#include <fcntl.h> // Provides open flags for command redirection.
#include <unistd.h> // Provides fork/exec-adjacent descriptor, cwd, and pipe calls.
#include <sys/wait.h> // Provides waitpid and process-status decoding.
#include <sys/stat.h> // Provides stat/file-type inspection for redirection and script tests.
#include <signal.h> // Provides SIGINT, SIGTSTP, SIGTERM, and SIGCONT constants.
#include <cerrno> // Exposes errno values used when system calls fail or are interrupted.
#include <algorithm> // Provides all_of and max for script parsing and positional-variable sizing.
#include <cctype> // Provides isalpha/isalnum/isspace character classification.

namespace Terminator {

    struct PipeWrapper { // Keeps one pipeline's two POSIX pipe descriptors together.
        int fd[2]; // fd[0] is read end; fd[1] is write end, as defined by pipe().
    };

    ShellEngine::ShellEngine(HistoryManager* history_mgr, OutputCallback out_cb, int in_fd, int out_fd) // ctor sets up history, callbacks, fds
        : history_(history_mgr), output_cb_(out_cb), in_fd_(in_fd), out_fd_(out_fd) { // Copy shared references/callback and optional I/O descriptors into this tab shell.
        
        // Initialize working directory to current process directory
        char buf[PATH_MAX]; // Buffer large enough for the platform's maximum current-directory string.
        if (getcwd(buf, sizeof(buf))) { // getcwd returns this buffer on success and null on failure.
            cwd_ = buf; // Store the real initial directory; shells later update cwd_ without changing other tabs.
        } else {
            cwd_ = "/";
        }
        oldpwd_ = cwd_;

        // Create pipe for interrupting multiWatch or operations via Ctrl+C
        if (pipe(cancel_pipe_) == 0) { // Create a private wakeup channel for cancelling blocking monitor loops.
            fcntl(cancel_pipe_[0], F_SETFL, O_NONBLOCK); // Reads return immediately when no cancellation byte is queued.
            fcntl(cancel_pipe_[1], F_SETFL, O_NONBLOCK); // Signal writes cannot indefinitely block the GUI-facing path.
        } else {
            cancel_pipe_[0] = -1;
            cancel_pipe_[1] = -1;
        }
    }

    ShellEngine::~ShellEngine() {
        jobs_.terminateAll();
        if (cancel_pipe_[0] >= 0) close(cancel_pipe_[0]); // Close the cancellation read endpoint if pipe creation succeeded.
        if (cancel_pipe_[1] >= 0) close(cancel_pipe_[1]); // Close its matching write endpoint.
    }

    void ShellEngine::print(const std::string& msg) {
        if (output_cb_) { // A callback may be absent in limited test or teardown contexts.
            output_cb_(msg); // Route text to the GUI buffer/IPC without writing Xlib code in the shell.
        }
    }

    void ShellEngine::pollJobs() {
        std::string notices; // Buffer status changes detected by waitpid.
        jobs_.updateStatus(notices); // Poll tracked children without blocking normal shell input.
        if (!notices.empty()) { // Avoid emitting an empty callback call.
            print(notices); // Send user-visible stopped/done messages to this tab.
        }
    }

    void ShellEngine::handleCtrlC() {
        // If multiWatch is actively running, signal it to stop
        if (multiwatch_active_ && current_multiwatch_) {
            current_multiwatch_->stop();
            if (cancel_pipe_[1] >= 0) { // Wake any wait that watches the fallback cancellation pipe.
                char dummy = 'C'; // A nonzero byte marks the cancellation request.
                ssize_t res = write(cancel_pipe_[1], &dummy, 1); // Best-effort nonblocking notification.
                (void)res; // Ignore a full pipe; stop flag already records the request.
            }
        }

        // If a foreground process group is running, signal it with SIGINT
        if (fg_pgid_ > 0) { // Zero means the shell has no current foreground process group.
            kill(-fg_pgid_, SIGINT); // Negative PID targets every process in that group's pipeline.
        }
    }

    void ShellEngine::handleCtrlZ() {
        // If a foreground process group is running, suspend it with SIGTSTP
        if (fg_pgid_ > 0) { // Only a running foreground command should receive Ctrl+Z.
            kill(-fg_pgid_, SIGTSTP); // Suspend the complete foreground process group.
        }
    }

    std::string ShellEngine::executeSubshell(const std::string& cmd_str) {
        // Execute subshell command (for command substitution) and capture stdout
        int pfd[2]; // Temporary capture pipe: child writes command output; parent reads it.
        if (pipe(pfd) < 0) return ""; // Return empty substitution output if descriptor creation fails.

        pid_t pid = fork(); // Duplicate the process to run substitution separately from the interactive shell.
        if (pid < 0) { // Negative return means process creation failed.
            close(pfd[0]);
            close(pfd[1]);
            return "";
        }

        if (pid == 0) { // Child branch executes the substitution command.
            close(pfd[0]); // Child does not read its own output.
            dup2(pfd[1], STDOUT_FILENO); // Route child stdout into the pipe for capture.
            close(pfd[1]); // stdout now owns a duplicate reference to the write endpoint.

            // Child switches to current tab directory
            if (chdir(cwd_.c_str()) != 0) {
                // Ignore failure
            }
            for (const auto& variable : variables_) setenv(variable.first.c_str(), variable.second.c_str(), 1); // Make this shell's local variables visible to the substitution child only.

            // Execute subshell command
            execl("/bin/sh", "sh", "-c", cmd_str.c_str(), (char*)nullptr); // Replace child image with a command interpreter for substitution text.
            _exit(127); // exec returns only on failure; _exit avoids flushing inherited parent stdio buffers.
        }

        close(pfd[1]); // Parent only reads; closing its writer allows EOF after child exit.
        std::string result; // Accumulate captured stdout in an owned string.
        char buffer[1024]; // Bounded temporary read chunk.
        ssize_t n;
        while ((n = read(pfd[0], buffer, sizeof(buffer))) > 0) { // Drain until EOF so a large child output cannot deadlock the child.
            result.append(buffer, n);
        }
        close(pfd[0]);

        int status; // waitpid writes encoded exit information; substitution currently uses stdout only.
        waitpid(pid, &status, 0); // Reap the child and wait for its process to finish.
        return result; // Return captured text for Parser to substitute into the original command.
    }

    void ShellEngine::execute(const std::string& command_line) {
        pollJobs(); // Announce any jobs that finished since the previous shell operation.

        std::string trimmed = trim(command_line); // Remove outer whitespace before recognizing special built-ins.
        if (trimmed.empty()) return; // Ignore blank input without forking or changing exit status.

        // Check if command is multiWatch ["cmd1", "cmd2", ...]
        std::vector<std::string> mw_cmds = Parser::parseMultiWatch(trimmed);
        if (!mw_cmds.empty()) {
            multiwatch_active_ = true; // Let Ctrl+C identify this special blocking operation.
            // Use in_fd_ if available for Ctrl+C detection, otherwise cancel_pipe_[0]
            int watch_cancel_fd = (in_fd_ >= 0) ? in_fd_ : cancel_pipe_[0];
            MultiWatchRunner mw(mw_cmds, output_cb_, watch_cancel_fd, cwd_); // Local runner owns its monitored children and pipes.
            current_multiwatch_ = &mw; // Publish a temporary pointer so signal/control paths can call stop().

            mw.run(); // Block here while poll relays command output and waits for Ctrl+C.

            current_multiwatch_ = nullptr; // The local runner is about to be destroyed and must no longer be referenced.
            multiwatch_active_ = false; // Restore ordinary shell signal handling.
            last_exit_status_ = 0;
            return;
        }

        // Subshell runner lambda for command substitution $(...)
        auto subshell_runner = [this](const std::string& s) { // Adapt parser's callback to this shell's cwd and variable state.
            return executeSubshell(s); // Return captured command output to substitution expansion.
        };

        // Parse command pipeline
        Pipeline pipeline = Parser::parsePipeline(trimmed, last_exit_status_, subshell_runner, &variables_); // Parse substitutions/variables/quotes into executable stages.
        if (pipeline.commands.empty()) return;

        // If pipeline has exactly 1 command with no pipes or background, check for builtins
        if (pipeline.commands.size() == 1 && !pipeline.run_in_background) {
            const auto& cmd = pipeline.commands[0];
            bool is_builtin = false;
            if (!cmd.args.empty()) {
                const std::string& n = cmd.args[0];
                if (n == "cd" || n == "history" || n == "jobs" || 
                    n == "fg" || n == "bg" || n == "source" || 
                    n == "pwd" || n == "exit" || n == "bash" ||
                    (n.size() > 3 && n.substr(n.size() - 3) == ".sh")) {
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
        if (name == "bash" || (name.size() > 3 && name.substr(name.size() - 3) == ".sh")) {
            std::vector<std::string> source_args{"source"};
            if (name == "bash") {
                if (cmd.args.size() < 2) {
                    print("terminator: bash: filename argument required\n");
                    last_exit_status_ = 2;
                    return true;
                }
                source_args.insert(source_args.end(), cmd.args.begin() + 1, cmd.args.end());
            } else {
                source_args.push_back(name);
                source_args.insert(source_args.end(), cmd.args.begin() + 1, cmd.args.end());
            }
            builtinSource(source_args);
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
        size_t first = entries.size() > 1000 ? entries.size() - 1000 : 0;
        for (size_t i = first; i < entries.size(); ++i) {
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
        if (args.size() < 2) { print("terminator: source: filename argument required\n"); last_exit_status_ = 1; return; }
        std::string script_path = expandTilde(args[1]);
        if (!isAbsolutePath(script_path)) script_path = (cwd_ == "/") ? ("/" + script_path) : (cwd_ + "/" + script_path);
        std::ifstream file(script_path);
        if (!file) { print("terminator: source: " + args[1] + ": No such file or directory\n"); last_exit_status_ = 1; return; }

        std::vector<std::string> lines; std::string line;
        while (std::getline(file, line)) lines.push_back(line);
        std::vector<bool> saw_else;
        bool invalid_structure = false;
        for (const auto& raw : lines) {
            std::string t = trim(raw);
            if (t.empty() || t[0] == '#') continue;
            if (t.rfind("if ", 0) == 0) saw_else.push_back(false);
            else if (t.rfind("elif ", 0) == 0) {
                if (saw_else.empty() || saw_else.back()) invalid_structure = true;
            } else if (t == "else") {
                if (saw_else.empty() || saw_else.back()) invalid_structure = true;
                else saw_else.back() = true;
            } else if (t == "fi" || t.rfind("fi;", 0) == 0) {
                if (saw_else.empty()) invalid_structure = true;
                else saw_else.pop_back();
            }
        }
        if (invalid_structure || !saw_else.empty()) {
            print("terminator: " + args[1] + ": syntax error: unmatched or misplaced if/else/fi\n");
            last_exit_status_ = 2;
            return;
        }

        size_t positional_slots = std::max<size_t>(9, args.size() > 1 ? args.size() - 1 : 0);
        std::vector<std::pair<bool, std::string>> old_positional;
        for (size_t i = 1; i <= positional_slots; ++i) {
            auto found = variables_.find(std::to_string(i));
            old_positional.push_back(found == variables_.end() ? std::make_pair(false, std::string())
                                                                 : std::make_pair(true, found->second));
            variables_.erase(std::to_string(i));
        }
        auto old_count_it = variables_.find("#");
        bool had_old_count = old_count_it != variables_.end();
        std::string saved_count = had_old_count ? old_count_it->second : std::string();
        for (size_t i = 2; i < args.size(); ++i) variables_[std::to_string(i - 1)] = args[i];
        variables_["#"] = std::to_string(args.size() - 2);

        auto testCondition = [&](std::string cond) {
            cond = trim(cond);
            if (cond.rfind("if ", 0) == 0) cond = trim(cond.substr(3));
            size_t then_pos = cond.find("; then");
            if (then_pos != std::string::npos) cond = cond.substr(0, then_pos);
            if (cond.size() >= 5 && cond.substr(0,2) == "[ " && cond.substr(cond.size()-2) == " ]") cond = cond.substr(2, cond.size()-4);
            if (!cond.empty() && cond.back() == ';') cond.pop_back();
            cond = Parser::expandVariables(cond, last_exit_status_, &variables_);
            auto t = Parser::tokenize(cond);
            if (!t.empty() && t.front() == "[") t.erase(t.begin());
            if (!t.empty() && t.back() == "]") t.pop_back();
            if (t.size() < 2) return false;
            if (t.size() >= 3) {
                const std::string& lhs=t[0]; const std::string& op=t[1]; const std::string& rhs=t[2];
                if (op == "=") return lhs == rhs;
                if (op == "!=") return lhs != rhs;
                try { long x=std::stol(lhs), y=std::stol(rhs); if(op=="-eq")return x==y; if(op=="-ne")return x!=y; if(op=="-gt")return x>y; if(op=="-lt")return x<y; if(op=="-ge")return x>=y; if(op=="-le")return x<=y; } catch (...) { return false; }
            }
            const std::string& path=t.back(); struct stat st;
            std::string resolved_path = path;
            if (!isAbsolutePath(resolved_path)) resolved_path = (cwd_ == "/" ? "/" : cwd_ + "/") + resolved_path;
            if (t[0] == "-f") return stat(resolved_path.c_str(), &st)==0 && S_ISREG(st.st_mode);
            if (t[0] == "-d") return stat(resolved_path.c_str(), &st)==0 && S_ISDIR(st.st_mode);
            return !path.empty();
        };

        bool script_exit = false;
        std::function<size_t(size_t,size_t)> runRange;
        runRange = [&](size_t begin, size_t end) -> size_t {
            size_t i=begin;
            while (i<end && !script_exit) {
                std::string raw=lines[i], t=trim(raw);
                if (t.empty() || t[0]=='#' || t=="then") { ++i; continue; }
                if (t.rfind("if ",0)==0) {
                    std::vector<std::pair<std::string,std::pair<size_t,size_t>>> branches;
                    std::string condition=t.substr(3); size_t body=i+1, j=body; int nested=0; size_t branch_start=body;
                    while (j<end) {
                        std::string jt=trim(lines[j]);
                        if (jt.rfind("if ",0)==0) ++nested;
                        if (nested==0 && (jt.rfind("elif ",0)==0 || jt=="else" || jt=="fi")) {
                            branches.push_back({condition,{branch_start,j}});
                            if (jt.rfind("elif ",0)==0) { condition=jt.substr(5); branch_start=j+1; }
                            else if (jt=="else") { condition=""; branch_start=j+1; }
                            else { ++j; break; }
                        } else if (jt=="fi") --nested;
                        ++j;
                    }
                    bool selected=false;
                    for (const auto& branch : branches) {
                        if (branch.first.empty() || testCondition(branch.first)) { runRange(branch.second.first,branch.second.second); selected=true; break; }
                    }
                    (void)selected; i=j; continue;
                }
                if (t=="fi" || t=="then" || t=="else" || t.rfind("elif ",0)==0) { ++i; continue; }
                if (t.rfind("exit",0)==0 && (t.size()==4 || std::isspace((unsigned char)t[4]))) {
                    auto tok=Parser::tokenize(t); if(tok.size()>1) { try { last_exit_status_=std::stoi(tok[1]); } catch(...) { last_exit_status_=2; } }
                    script_exit=true; ++i; continue;
                }
                size_t eq=t.find('=');
                size_t ws=t.find_first_of(" \t");
                if(eq!=std::string::npos && eq>0 && (ws==std::string::npos || eq<ws)) {
                    std::string key=t.substr(0,eq), value=t.substr(eq+1);
                    if(ws!=std::string::npos) value=t.substr(eq+1);
                    if((std::isalpha(static_cast<unsigned char>(key[0])) || key[0]=='_') &&
                       std::all_of(key.begin(),key.end(),[](unsigned char c){return std::isalnum(c)||c=='_';})) {
                        value=Parser::expandVariables(value,last_exit_status_,&variables_);
                        auto assignment_words = Parser::tokenize(value);
                        if (assignment_words.size() == 1) value = assignment_words.front();
                        else if (assignment_words.empty()) value.clear();
                        variables_[key]=value; last_exit_status_=0; ++i; continue;
                    }
                }
                execute(raw); ++i;
            }
            return i;
        };
        runRange(0,lines.size());
        for (size_t i = 1; i <= positional_slots; ++i) {
            if (old_positional[i - 1].first) variables_[std::to_string(i)] = old_positional[i - 1].second;
            else variables_.erase(std::to_string(i));
        }
        if (had_old_count) variables_["#"] = saved_count; else variables_.erase("#");
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
                for (const auto& variable : variables_) setenv(variable.first.c_str(), variable.second.c_str(), 1);

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
// EOF
