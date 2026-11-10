#include "tab.hpp" // Declares Tab state and the ShellEngine/history interfaces it connects.
#include <unistd.h> // Provides pipe, read, write, close, getcwd, and _exit system calls.
#include <fcntl.h> // Provides fcntl flags for making the GUI's pipe reads nonblocking.
#include <sys/wait.h> // Provides waitpid for reaping process-mode tab workers.
#include <signal.h> // Provides signal handlers and POSIX signal-action structures.
#include <algorithm> // Provides line-buffer trimming and scroll-bound helpers.
#include <cctype> // Provides character classification for editing operations.

namespace Terminator {

    static ShellEngine* g_tab_shell = nullptr; // Thread/process-local pointer used by signal handlers to reach the active shell.

    static void tab_sigint_handler(int sig) { // Receive SIGINT in a process-mode tab and forward it to its foreground job.
        (void)sig; // The shell only needs to know that an interrupt arrived, not which numeric value caused this handler.
        if (g_tab_shell) { // A null pointer means the shell engine has not started or has already been torn down.
            g_tab_shell->handleCtrlC(); // Ask ShellEngine to signal its active process group rather than exiting itself.
        }
    }

    static void tab_sigtstp_handler(int sig) { // Receive SIGTSTP and translate it into job suspension behavior.
        (void)sig; // Suppress the unused-parameter warning while keeping the POSIX handler signature.
        if (g_tab_shell) { // Only forward while a live shell object is registered.
            g_tab_shell->handleCtrlZ(); // Signal the foreground job group through shell job-control logic.
        }
    }

    Tab::Tab(int id, bool use_process_mode, HistoryManager* history_mgr) // Initialize identity/shared references and launch its worker.
        : id_(id), use_process_mode_(use_process_mode), history_(history_mgr) {
        
        char cwd[PATH_MAX]; // Stack buffer passed to getcwd for the initial tab title.
        title_ = getcwd(cwd, sizeof(cwd)) ? shortenPath(cwd) : "~"; // Label the new tab with actual cwd, shortened relative to HOME when possible.

        if (use_process_mode_) { // Process mode isolates tab crashes and shell state in a child process.
            startProcessMode(); // Build two IPC pipes and fork the worker.
        } else {
            startThreadMode(); // Build two IPC pipes and launch a pthread worker in this process.
        }
    }

    Tab::~Tab() {
        terminate(); // Ensure worker and descriptors are cleaned up even if GUI forgot to close explicitly.
    }

    std::string Tab::getPrompt() const {
        if (is_continuation_) { // Incomplete quotes/substitutions need a visually distinct continuation prompt.
            return "> "; // Match the assignment's continuation prompt.
        }
        return "user@terminator> "; // Normal prompt shown before the first line of a command.
    }

    void Tab::startThreadMode() {
        // Create bidirectional pipes between GUI and Shell worker thread
        int p_to_tab[2]; // GUI-to-worker pipe: index 1 is the GUI writer, index 0 the worker reader.
        int p_from_tab[2]; // Worker-to-GUI pipe: index 1 is the worker writer, index 0 the GUI reader.

        if (pipe(p_to_tab) < 0 || pipe(p_from_tab) < 0) { // pipe() fills each two-element array with read/write descriptors.
            perror("tab thread pipes");
            return;
        }

        to_tab_fd_ = p_to_tab[1]; // Retain only the GUI-side write end for sending commands.
        from_tab_fd_ = p_from_tab[0]; // Retain only the GUI-side read end for collecting output.

        // Set non-blocking on GUI read end
        fcntl(from_tab_fd_, F_SETFL, O_NONBLOCK); // A quiet tab must not block the GUI event loop on read().

        struct ThreadContext {
            Tab* tab;
            int in_fd;
            int out_fd;
        };

        ThreadContext* ctx = new ThreadContext{this, p_to_tab[0], p_from_tab[1]}; // Heap context survives until the asynchronous pthread starts and deletes it.

        pthread_create(&thread_handle_, nullptr, [](void* p) -> void* {
            ThreadContext* c = static_cast<ThreadContext*>(p); // Recover the typed argument passed through pthread_create's void pointer.
            c->tab->runShellLoop(c->in_fd, c->out_fd); // Execute the shell loop on this worker thread.
            close(c->in_fd); // Worker owns and closes its input read end after loop completion.
            close(c->out_fd); // Worker owns and closes its output write end after loop completion.
            delete c; // Release the heap context allocated by startThreadMode.
            return nullptr; // pthread entry points return a void pointer; no result is needed.
        }, ctx);
    }

    void Tab::startProcessMode() {
        // In process mode, each tab fork()s its own process and communicates with GUI via pipes
        int p_to_tab[2]; // Command pipe endpoints passed through fork.
        int p_from_tab[2]; // Output pipe endpoints passed through fork.

        if (pipe(p_to_tab) < 0 || pipe(p_from_tab) < 0) {
            perror("tab process pipes");
            return;
        }

        pid_t pid = fork(); // Duplicate the GUI process; child gets 0 while parent receives the child's PID.
        if (pid < 0) { // Negative return means no process was created.
            perror("tab fork");
            return;
        }

        if (pid == 0) { // Child branch becomes the shell worker for this tab.
            // Child process: set independent process group
            setpgid(0, 0); // Isolate this tab in its own process group for tab-specific signals.

            signal(SIGPIPE, SIG_IGN); // Closing GUI readers must not terminate the worker asynchronously.

            // Install signal handlers so Ctrl+C and Ctrl+Z are caught and forwarded to children
            // rather than terminating the tab shell process
            struct sigaction sa_int; // POSIX action record describes the SIGINT callback and mask.
            sa_int.sa_handler = tab_sigint_handler; // Route interrupts to ShellEngine's foreground job handling.
            sigemptyset(&sa_int.sa_mask); // Do not block additional signals while this handler runs.
            sa_int.sa_flags = 0; // No SA_RESTART: blocking waits may return EINTR after the signal.
            sigaction(SIGINT, &sa_int, nullptr); // Install the action and discard the previous action.

            struct sigaction sa_tstp; // Separate action record for Ctrl+Z suspension requests.
            sa_tstp.sa_handler = tab_sigtstp_handler; // Forward the stop request into shell job control.
            sigemptyset(&sa_tstp.sa_mask); // Leave the handler mask empty.
            sa_tstp.sa_flags = 0; // Allow interrupted waits to notice this signal.
            sigaction(SIGTSTP, &sa_tstp, nullptr); // Install this process's SIGTSTP handler.

            close(p_to_tab[1]); // Child only reads commands, so close its inherited write copy.
            close(p_from_tab[0]); // Child only writes output, so close its inherited read copy.

            runShellLoop(p_to_tab[0], p_from_tab[1]); // Run command parsing/execution until GUI closes the input pipe.

            close(p_to_tab[0]); // Release the worker's command input descriptor after the loop.
            close(p_from_tab[1]); // Release its output writer so GUI can observe EOF.
            _exit(0); // Exit child without running inherited parent-side C++ destructors.
        }

        // GUI parent process
        close(p_to_tab[0]); // Parent writes commands, so it closes the unused reader.
        close(p_from_tab[1]); // Parent reads output, so it closes the unused writer.

        to_tab_fd_ = p_to_tab[1]; // Save the GUI command writer.
        from_tab_fd_ = p_from_tab[0]; // Save the GUI output reader.
        tab_pid_ = pid; // Keep the worker PID for signals and waitpid during termination.

        // Non-blocking on GUI read end
        fcntl(from_tab_fd_, F_SETFL, O_NONBLOCK); // Ensure a read cannot freeze the X11 event loop waiting for output.
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
        if (shell_) { // Thread-mode shell pointer is directly reachable inside the same process.
            if (sig == SIGINT) { // Ctrl+C is represented by SIGINT.
                shell_->handleCtrlC(); // Forward to ShellEngine so only its foreground group is interrupted.
            } else if (sig == SIGTSTP) { // Ctrl+Z is represented by SIGTSTP.
                shell_->handleCtrlZ(); // Ask this shell to suspend its foreground process group.
            }
        }

        // Process mode: deliver signal directly to the tab process
        if (use_process_mode_ && tab_pid_ > 0) { // Process mode cannot call the child ShellEngine directly from the GUI.
            kill(tab_pid_, sig); // Deliver the signal to the tab worker, whose handler forwards it to its shell job.
        }

        // Secondary pipe channel
        if (to_tab_fd_ >= 0) { // Also use the control pipe so the worker loop can react to terminal-style control bytes.
            char c = (sig == SIGINT) ? '\003' : '\032'; // ETX and SUB are the internal bytes for Ctrl+C and Ctrl+Z.
            ssize_t res = write(to_tab_fd_, &c, 1); // Write one byte to the worker's input stream.
            (void)res; // This path intentionally ignores a short/error result during best-effort signal forwarding.
        }
    }

    void Tab::terminate() {
        is_closed_ = true; // Stop the worker loop from processing any more input.

        if (to_tab_fd_ >= 0) { // Close only descriptors that were initialized successfully.
            close(to_tab_fd_); // EOF wakes a worker blocked in read() once other writer copies are closed.
            to_tab_fd_ = -1; // Sentinel prevents accidental double-close.
        }

        if (from_tab_fd_ >= 0) { // The GUI owns this reader.
            close(from_tab_fd_); // Release its pipe endpoint.
            from_tab_fd_ = -1; // Mark it invalid for repeated terminate calls.
        }

        if (tab_pid_ > 0) { // A positive PID means a process-mode worker exists.
            kill(tab_pid_, SIGTERM); // Signal the worker directly.
            kill(-tab_pid_, SIGTERM); // Also signal its isolated process group and any descendants.
            int status; // waitpid output field for encoded child termination status.
            waitpid(tab_pid_, &status, WNOHANG); // Reap immediately if possible without blocking GUI shutdown.
            tab_pid_ = -1; // Prevent signaling this PID again after teardown.
        }

        if (thread_handle_ != 0) { // A nonzero pthread handle means thread mode created a joinable worker.
            pthread_join(thread_handle_, nullptr); // Wait until the worker finishes and releases its shell resources.
            thread_handle_ = 0; // Clear the handle so terminate remains idempotent.
        }
    }

    void Tab::appendText(const std::string& text) {
        if (text.empty()) return; // No buffer changes or redraw are needed for an empty output chunk.

        if (lines_.empty()) { // appendText expects a current line to append non-newline characters into.
            lines_.emplace_back(""); // Create that initial empty screen line.
        }

        bool in_esc = false; // Tracks basic terminal escape sequences so control bytes are not drawn as visible text.

        for (size_t i = 0; i < text.length(); ++i) {
            char c = text[i]; // Process one output byte received from the shell.

            if (in_esc) {
                if (std::isalpha(c) || c == 'm' || c == 'H' || c == 'J' || c == 'K') {
                    in_esc = false; // Recognized final byte ends this simple escape sequence.
                }
                continue; // Do not render bytes belonging to the escape sequence.
            }

            if (c == '\033') {
                in_esc = true; // Begin skipping an ANSI/OSC sequence at ESC.
                continue;
            }

            if (c == '\t') {
                // Expand tab characters to 4 spaces for proper column alignment
                lines_.back().append("    "); // Expand tabs to fixed spaces because XDrawString uses simple character cells.
            } else if (c == '\n') {
                lines_.emplace_back(""); // Newline advances the text buffer to a fresh row.
            } else if (c == '\r') {
                // Ignore carriage return
            } else if (static_cast<unsigned char>(c) >= 32) {
                lines_.back() += c; // Store printable bytes for later X11 rendering.
            }
        }

        // Limit screen buffer lines to avoid unbounded memory growth (keep last 5000 lines)
        if (lines_.size() > 5000) { // Bound scrollback to avoid unbounded memory growth during long sessions.
            lines_.erase(lines_.begin(), lines_.begin() + (lines_.size() - 5000));
        }

        // Reset scroll to bottom when new text arrives
        scroll_offset_ = 0; // New output follows the prompt, so return the viewport to the bottom.
    }

    void Tab::appendLine(const std::string& line) {
        lines_.push_back(line); // Add the completed text row to scrollback.
        // Prepare empty line for command output to avoid concatenating onto prompt line
        lines_.emplace_back(""); // Keep an editable/output row after the appended line.
        scroll_offset_ = 0; // Show the newest line immediately.
    }

    void Tab::clearScreen() {
        lines_.clear(); // Remove all stored screen rows.
        scroll_offset_ = 0; // Reset view to the bottom of the now-empty buffer.
    }

    void Tab::scroll(int delta_lines, int visible_rows) {
        scroll_offset_ += delta_lines; // Positive/negative input moves the viewport through scrollback.
        int max_scroll = (int)lines_.size() - visible_rows; // Compute the oldest valid viewport offset.
        if (max_scroll < 0) max_scroll = 0; // A buffer shorter than the viewport cannot scroll backward.

        if (scroll_offset_ < 0) scroll_offset_ = 0; // Clamp below the newest view.
        if (scroll_offset_ > max_scroll) scroll_offset_ = max_scroll; // Clamp above the oldest available view.
    }

    void Tab::insertChar(char c) {
        if (cursor_pos_ >= current_input_.length()) { // Fast path when typing at the end of the line.
            current_input_ += c; // Append the new byte to the input string.
            cursor_pos_ = current_input_.length(); // Keep cursor immediately after the appended byte.
        } else {
            current_input_.insert(cursor_pos_, 1, c); // Insert before the existing suffix while preserving that suffix.
            cursor_pos_++; // Advance past the inserted character.
        }
    }

    void Tab::backspace() {
        if (cursor_pos_ > 0 && !current_input_.empty()) { // Backspace is valid only when a character exists to the left.
            current_input_.erase(cursor_pos_ - 1, 1); // Remove exactly the preceding character.
            cursor_pos_--; // Move cursor to the gap where that character was.
        }
    }

    void Tab::deleteChar() {
        if (cursor_pos_ < current_input_.length()) { // Delete has an effect only when a character is under the cursor.
            current_input_.erase(cursor_pos_, 1); // Remove that character while leaving the cursor index in place.
        }
    }

    void Tab::moveCursorLeft() {
        if (cursor_pos_ > 0) cursor_pos_--; // Stay at the start instead of wrapping around.
    }

    void Tab::moveCursorRight() {
        if (cursor_pos_ < current_input_.length()) cursor_pos_++; // Stay at the end instead of moving beyond valid string indices.
    }

    void Tab::moveCursorStart() {
        cursor_pos_ = 0; // String index zero is the beginning of the line.
    }

    void Tab::moveCursorEnd() {
        cursor_pos_ = current_input_.length(); // One-past-last is the insertion point at line end.
    }

    void Tab::deleteToStart() {
        if (cursor_pos_ > 0) { // Only text before a nonzero cursor needs removal.
            current_input_.erase(0, cursor_pos_); // Erase the half-open range [0,cursor_pos_).
            cursor_pos_ = 0; // The preserved suffix now begins at the cursor.
        }
    }

    void Tab::deleteToEnd() {
        if (cursor_pos_ < current_input_.length()) { // Skip erase when the cursor is already at end.
            current_input_.erase(cursor_pos_); // Single-argument erase removes the suffix starting at cursor_pos_.
        }
    }

    void Tab::deletePrevWord() {
        if (cursor_pos_ == 0 || current_input_.empty()) return; // There is no preceding word to delete in this case.

        size_t p = cursor_pos_; // Work backward from the cursor without modifying the string yet.
        // Skip whitespace left of cursor
        while (p > 0 && std::isspace(current_input_[p - 1])) { // First skip any spaces directly before the cursor.
            p--; // Move left one byte while whitespace continues.
        }
        // Skip word characters
        while (p > 0 && !std::isspace(current_input_[p - 1])) { // Then continue left across the preceding word.
            p--; // Decrease start boundary until whitespace or line start.
        }

        current_input_.erase(p, cursor_pos_ - p); // Remove precisely the range from word start to the old cursor.
        cursor_pos_ = p; // Place the caret at the beginning of the removed range.
    }

    void Tab::historyPrev() {
        if (!history_ || history_->size() == 0) return; // No manager or no commands means there is nothing to recall.

        if (history_index_ == -1) {
            saved_input_before_browse_ = current_input_; // Preserve the user's unsent draft before replacing it with history.
            history_index_ = (int)history_->size() - 1; // Start at the newest (last) chronological history entry.
        } else if (history_index_ > 0) {
            history_index_--; // Move toward older entries while avoiding a negative index.
        }

        if (history_index_ >= 0) {
            current_input_ = history_->get(history_index_); // Copy the selected command into this tab's editable buffer.
            cursor_pos_ = current_input_.length(); // Place cursor at the end of the recalled command.
        }
    }

    void Tab::historyNext() {
        if (!history_ || history_index_ == -1) return; // Down does nothing unless a history browse is active.

        if (history_index_ < (int)history_->size() - 1) {
            history_index_++; // Move toward newer commands.
            current_input_ = history_->get(history_index_); // Replace current text with the next stored command.
            cursor_pos_ = current_input_.length(); // Keep the cursor at the end after navigation.
        } else {
            history_index_ = -1; // Leave history mode once the newest entry has been passed.
            current_input_ = saved_input_before_browse_; // Restore exactly what the user had typed before browsing.
            cursor_pos_ = current_input_.length(); // Restore the draft with cursor at its end.
        }
    }

} // namespace Terminator
// EOF
