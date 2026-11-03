#include "tab.hpp"
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <algorithm>
#include <cctype>

namespace Terminator {

    static ShellEngine* g_tab_shell = nullptr;

    static void tab_sigint_handler(int sig) {
        (void)sig;
        if (g_tab_shell) {
            g_tab_shell->handleCtrlC();
        }
    }

    static void tab_sigtstp_handler(int sig) {
        (void)sig;
        if (g_tab_shell) {
            g_tab_shell->handleCtrlZ();
        }
    }

    Tab::Tab(int id, bool use_process_mode, HistoryManager* history_mgr)
        : id_(id), use_process_mode_(use_process_mode), history_(history_mgr) {
        
        title_ = "~";

        if (use_process_mode_) {
            startProcessMode();
        } else {
            startThreadMode();
        }
    }

    Tab::~Tab() {
        terminate();
    }

    std::string Tab::getPrompt() const {
        if (is_continuation_) {
            return "> ";
        }
        return "user@terminator> ";
    }

    void Tab::startThreadMode() {
        // Create bidirectional pipes between GUI and Shell worker thread
        int p_to_tab[2];
        int p_from_tab[2];

        if (pipe(p_to_tab) < 0 || pipe(p_from_tab) < 0) {
            perror("tab thread pipes");
            return;
        }

        to_tab_fd_ = p_to_tab[1];
        from_tab_fd_ = p_from_tab[0];

        // Set non-blocking on GUI read end
        fcntl(from_tab_fd_, F_SETFL, O_NONBLOCK);

        struct ThreadContext {
            Tab* tab;
            int in_fd;
            int out_fd;
        };

        ThreadContext* ctx = new ThreadContext{this, p_to_tab[0], p_from_tab[1]};

        pthread_create(&thread_handle_, nullptr, [](void* p) -> void* {
            ThreadContext* c = static_cast<ThreadContext*>(p);
            c->tab->runShellLoop(c->in_fd, c->out_fd);
            close(c->in_fd);
            close(c->out_fd);
            delete c;
            return nullptr;
        }, ctx);
    }

    void Tab::startProcessMode() {
        // In process mode, each tab fork()s its own process and communicates with GUI via pipes
        int p_to_tab[2];
        int p_from_tab[2];

        if (pipe(p_to_tab) < 0 || pipe(p_from_tab) < 0) {
            perror("tab process pipes");
            return;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("tab fork");
            return;
        }

        if (pid == 0) {
            // Child process: set independent process group
            setpgid(0, 0);

            signal(SIGPIPE, SIG_IGN);

            // Install signal handlers so Ctrl+C and Ctrl+Z are caught and forwarded to children
            // rather than terminating the tab shell process
            struct sigaction sa_int;
            sa_int.sa_handler = tab_sigint_handler;
            sigemptyset(&sa_int.sa_mask);
            sa_int.sa_flags = 0; // ensure waitpid returns EINTR
            sigaction(SIGINT, &sa_int, nullptr);

            struct sigaction sa_tstp;
            sa_tstp.sa_handler = tab_sigtstp_handler;
            sigemptyset(&sa_tstp.sa_mask);
            sa_tstp.sa_flags = 0;
            sigaction(SIGTSTP, &sa_tstp, nullptr);

            close(p_to_tab[1]);
            close(p_from_tab[0]);

            runShellLoop(p_to_tab[0], p_from_tab[1]);

            close(p_to_tab[0]);
            close(p_from_tab[1]);
            _exit(0);
        }

        // GUI parent process
        close(p_to_tab[0]);
        close(p_from_tab[1]);

        to_tab_fd_ = p_to_tab[1];
        from_tab_fd_ = p_from_tab[0];
        tab_pid_ = pid;

        // Non-blocking on GUI read end
        fcntl(from_tab_fd_, F_SETFL, O_NONBLOCK);
    }

    void Tab::runShellLoop(int in_fd, int out_fd) {
        // Output callback sending output back across pipe to GUI
        auto out_cb = [out_fd](const std::string& str) {
            ssize_t written = 0;
            size_t total = str.length();
            const char* ptr = str.c_str();
            while (written < (ssize_t)total) {
                ssize_t n = write(out_fd, ptr + written, total - written);
                if (n <= 0) break;
                written += n;
            }
        };

        // In process mode, redirect stdout/stderr for the tab child process
        if (use_process_mode_) {
            dup2(out_fd, STDOUT_FILENO);
            dup2(out_fd, STDERR_FILENO);
        }

        ShellEngine shell(history_, out_cb, in_fd, out_fd);
        shell_ = &shell;
        g_tab_shell = &shell;

        // Command read loop
        std::string command_buffer;
        char buf[512];

        while (!is_closed_) {
            ssize_t n = read(in_fd, buf, sizeof(buf));
            if (n <= 0) {
                break; // Pipe closed
            }

            for (ssize_t i = 0; i < n; ++i) {
                char c = buf[i];
                if (c == '\003') { // Signal Ctrl+C
                    shell.handleCtrlC();
                } else if (c == '\032') { // Signal Ctrl+Z
                    shell.handleCtrlZ();
                } else if (c == '\n') {
                    shell.execute(command_buffer);
                    command_buffer.clear();
                    // Signal prompt ready
                    out_cb("\033]PROMPT\007");
                } else {
                    command_buffer += c;
                }
            }
        }

        shell_ = nullptr;
        g_tab_shell = nullptr;
    }

    void Tab::sendCommand(const std::string& cmd) {
        if (to_tab_fd_ >= 0) {
            std::string line = cmd + "\n";
            ssize_t res = write(to_tab_fd_, line.c_str(), line.length());
            (void)res;
        }
    }

    void Tab::sendSignal(int sig) {
        // Direct dispatch if shell pointer is accessible in thread mode
        if (shell_) {
            if (sig == SIGINT) {
                shell_->handleCtrlC();
            } else if (sig == SIGTSTP) {
                shell_->handleCtrlZ();
            }
        }

        // Process mode: deliver signal directly to the tab process
        if (use_process_mode_ && tab_pid_ > 0) {
            kill(tab_pid_, sig);
        }

        // Secondary pipe channel
        if (to_tab_fd_ >= 0) {
            char c = (sig == SIGINT) ? '\003' : '\032';
            ssize_t res = write(to_tab_fd_, &c, 1);
            (void)res;
        }
    }

    void Tab::terminate() {
        is_closed_ = true;

        if (to_tab_fd_ >= 0) {
            close(to_tab_fd_);
            to_tab_fd_ = -1;
        }

        if (from_tab_fd_ >= 0) {
            close(from_tab_fd_);
            from_tab_fd_ = -1;
        }

        if (tab_pid_ > 0) {
            kill(tab_pid_, SIGTERM);
            kill(-tab_pid_, SIGTERM);
            int status;
            waitpid(tab_pid_, &status, WNOHANG);
            tab_pid_ = -1;
        }

        if (thread_handle_ != 0) {
            pthread_join(thread_handle_, nullptr);
            thread_handle_ = 0;
        }
    }

    void Tab::appendText(const std::string& text) {
        if (text.empty()) return;

        if (lines_.empty()) {
            lines_.emplace_back("");
        }

        bool in_esc = false;

        for (size_t i = 0; i < text.length(); ++i) {
            char c = text[i];

            if (in_esc) {
                if (std::isalpha(c) || c == 'm' || c == 'H' || c == 'J' || c == 'K') {
                    in_esc = false;
                }
                continue;
            }

            if (c == '\033') {
                in_esc = true;
                continue;
            }

            if (c == '\t') {
                // Expand tab characters to 4 spaces for proper column alignment
                lines_.back().append("    ");
            } else if (c == '\n') {
                lines_.emplace_back("");
            } else if (c == '\r') {
                // Ignore carriage return
            } else if (static_cast<unsigned char>(c) >= 32) {
                lines_.back() += c;
            }
        }

        // Limit screen buffer lines to avoid unbounded memory growth (keep last 5000 lines)
        if (lines_.size() > 5000) {
            lines_.erase(lines_.begin(), lines_.begin() + (lines_.size() - 5000));
        }

        // Reset scroll to bottom when new text arrives
        scroll_offset_ = 0;
    }

    void Tab::appendLine(const std::string& line) {
        lines_.push_back(line);
        // Prepare empty line for command output to avoid concatenating onto prompt line
        lines_.emplace_back("");
        scroll_offset_ = 0;
    }

    void Tab::clearScreen() {
        lines_.clear();
        scroll_offset_ = 0;
    }

    void Tab::scroll(int delta_lines, int visible_rows) {
        scroll_offset_ += delta_lines;
        int max_scroll = (int)lines_.size() - visible_rows;
        if (max_scroll < 0) max_scroll = 0;

        if (scroll_offset_ < 0) scroll_offset_ = 0;
        if (scroll_offset_ > max_scroll) scroll_offset_ = max_scroll;
    }

    void Tab::insertChar(char c) {
        if (cursor_pos_ >= current_input_.length()) {
            current_input_ += c;
            cursor_pos_ = current_input_.length();
        } else {
            current_input_.insert(cursor_pos_, 1, c);
            cursor_pos_++;
        }
    }

    void Tab::backspace() {
        if (cursor_pos_ > 0 && !current_input_.empty()) {
            current_input_.erase(cursor_pos_ - 1, 1);
            cursor_pos_--;
        }
    }

    void Tab::deleteChar() {
        if (cursor_pos_ < current_input_.length()) {
            current_input_.erase(cursor_pos_, 1);
        }
    }

    void Tab::moveCursorLeft() {
        if (cursor_pos_ > 0) cursor_pos_--;
    }

    void Tab::moveCursorRight() {
        if (cursor_pos_ < current_input_.length()) cursor_pos_++;
    }

    void Tab::moveCursorStart() {
        cursor_pos_ = 0;
    }

    void Tab::moveCursorEnd() {
        cursor_pos_ = current_input_.length();
    }

    void Tab::deleteToStart() {
        if (cursor_pos_ > 0) {
            current_input_.erase(0, cursor_pos_);
            cursor_pos_ = 0;
        }
    }

    void Tab::deleteToEnd() {
        if (cursor_pos_ < current_input_.length()) {
            current_input_.erase(cursor_pos_);
        }
    }

    void Tab::deletePrevWord() {
        if (cursor_pos_ == 0 || current_input_.empty()) return;

        size_t p = cursor_pos_;
        // Skip whitespace left of cursor
        while (p > 0 && std::isspace(current_input_[p - 1])) {
            p--;
        }
        // Skip word characters
        while (p > 0 && !std::isspace(current_input_[p - 1])) {
            p--;
        }

        current_input_.erase(p, cursor_pos_ - p);
        cursor_pos_ = p;
    }

    void Tab::historyPrev() {
        if (!history_ || history_->size() == 0) return;

        if (history_index_ == -1) {
            saved_input_before_browse_ = current_input_;
            history_index_ = (int)history_->size() - 1;
        } else if (history_index_ > 0) {
            history_index_--;
        }

        if (history_index_ >= 0) {
            current_input_ = history_->get(history_index_);
            cursor_pos_ = current_input_.length();
        }
    }

    void Tab::historyNext() {
        if (!history_ || history_index_ == -1) return;

        if (history_index_ < (int)history_->size() - 1) {
            history_index_++;
            current_input_ = history_->get(history_index_);
            cursor_pos_ = current_input_.length();
        } else {
            history_index_ = -1;
            current_input_ = saved_input_before_browse_;
            cursor_pos_ = current_input_.length();
        }
    }

} // namespace Terminator
