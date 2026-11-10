#ifndef TERMINATOR_TAB_HPP // Protect the Tab class declaration from duplicate inclusion.
#define TERMINATOR_TAB_HPP // Define this header's unique guard.

#include "common.hpp" // Supplies process/descriptor types and terminal constants.
#include "shell.hpp" // Each tab runs a ShellEngine to parse commands and manage its jobs.
#include "history.hpp" // Tabs share the GUI-owned command-history manager.
#include <string> // Stores the title, current input, and terminal text.
#include <vector> // Holds scrollback lines for drawing by TerminalGUI.
#include <pthread.h> // Declares pthread handles and routines for thread-mode tabs.

namespace Terminator {

    /**
     * Tab encapsulates all state and communications for a single terminal tab.
     * Manages:
     * - Line buffer for terminal output and scrolling
     * - Active input buffer, cursor position, multiline continuation
     * - IPC pipes between GUI and Tab Shell
     * - Thread / Process lifecycle depending on execution mode
     */
    class Tab { // Encapsulates one shell instance, its screen/input state, and worker communication.
    public:
        Tab(int id, bool use_process_mode, HistoryManager* history_mgr); // Assign identity and start either a thread worker or forked worker process.
        ~Tab(); // Terminate the worker and release both IPC pipe ends.

        // Prevent copying
        Tab(const Tab&) = delete; // A live tab owns unique descriptors and worker state, so copying is unsafe.
        Tab& operator=(const Tab&) = delete; // Prevent assignment from duplicating ownership of those resources.

        // Terminal text buffer operations
        void appendText(const std::string& text); // Split/append output text into the tab's scrollback buffer.
        void appendLine(const std::string& line); // Append one complete screen line.
        void clearScreen(); // Remove visible scrollback while preserving the active input line.
        void scroll(int delta_lines, int visible_rows); // Adjust the scroll offset while keeping it within valid bounds.

        // Input & line editing operations
        void insertChar(char c); // Insert at the cursor and shift the remaining input right.
        void backspace(); // Remove the character immediately before the cursor.
        void deleteChar(); // Remove the character under the cursor without moving it.
        void moveCursorLeft(); // Move the insertion point left by one character when possible.
        void moveCursorRight(); // Move the insertion point right by one character when possible.
        void moveCursorStart(); // Place the cursor at index zero.
        void moveCursorEnd(); // Place the cursor at the input string's end.
        void deleteToStart(); // Remove text before the cursor, preserving the suffix.
        void deleteToEnd(); // Remove text after the cursor, preserving the prefix.
        void deletePrevWord(); // Remove whitespace and the preceding word before the cursor.

        // History browsing
        void historyPrev(); // Replace current input with the previous shared-history item.
        void historyNext(); // Move forward in history or restore the user's draft input.

        // Tab lifecycle & signals
        void sendCommand(const std::string& cmd); // Write a command plus framing/newline bytes to the worker input pipe.
        void sendSignal(int sig); // Forward Ctrl+C/Ctrl+Z intent to the thread shell or child process.
        void terminate(); // Mark closed, close IPC, signal/wait for a process or join its worker thread.

        // Accessors
        int getId() const { return id_; } // Return the GUI-assigned tab number.
        std::string getTitle() const { return title_; } // Return the current cwd label drawn in the tab bar.
        void setTitle(const std::string& title) { title_ = title; } // Apply a title update sent by the shell through its output stream.
        const std::vector<std::string>& getLines() const { return lines_; } // Expose scrollback read-only to the renderer.
        const std::string& getCurrentInput() const { return current_input_; } // Expose editable text without copying it.
        size_t getCursorPos() const { return cursor_pos_; } // Return the cursor's character index in current_input_.
        int getScrollOffset() const { return scroll_offset_; } // Return how far the view is scrolled back from the latest line.
        bool isContinuation() const { return is_continuation_; } // Tell GUI whether the next line is continuing incomplete syntax.
        std::string getContinuationBuffer() const { return continuation_buffer_; } // Return accumulated multiline command text.
        void setContinuation(bool cont, const std::string& buf) { // Update the multiline-input state after an Enter keypress.
            is_continuation_ = cont; // true makes getPrompt() display the continuation prompt.
            continuation_buffer_ = buf; // Save earlier lines so they can be combined with the next input line.
        }
        void resetInput() { // Prepare a clean editable line after command submission or replacement.
            current_input_.clear(); // Remove all characters currently typed.
            cursor_pos_ = 0; // Return the cursor to the now-empty buffer's beginning.
            history_index_ = -1; // Exit history browsing mode.
            saved_input_before_browse_.clear(); // Discard any draft saved before navigating history.
        }

        int getReadFd() const { return from_tab_fd_; } // Read end polled by GUI to receive shell output.
        bool isClosed() const { return is_closed_; } // Report whether the worker has been shut down.
        void markClosed() { is_closed_ = true; } // Set the lifecycle flag when a worker reports termination.
        bool isExecuting() const { return is_executing_; } // Report whether a command is currently being processed.
        void setExecuting(bool val) { is_executing_ = val; } // Update the GUI-visible execution state.

        pid_t getPid() const { return tab_pid_; } // Return child PID in process mode; thread mode uses its pthread handle instead.
        std::string getPrompt() const; // Build the normal or continuation prompt shown before user input.

    private:
        void startThreadMode(); // Create GUI/worker pipes and launch the shell loop with pthread_create.
        void startProcessMode(); // Create IPC pipes and fork a shell worker process.
        static void* threadEntry(void* arg); // pthread-compatible trampoline that calls the instance's shell loop.
        void runShellLoop(int in_fd, int out_fd); // Read commands/signals, execute them, and write output in the worker.

        int id_; // GUI-assigned visible tab number.
        bool use_process_mode_; // Selects process-based or pthread-based shell worker creation.
        std::string title_; // Current directory/title text shown by the GUI.
        HistoryManager* history_; // Non-owning reference to history shared by the GUI and all tabs.

        // Screen buffer
        std::vector<std::string> lines_; // Screen scrollback lines collected from child or built-in output.
        std::string current_input_; // Command text currently being edited by the GUI.
        size_t cursor_pos_ = 0; // Character offset where the next typed character will be inserted.
        int scroll_offset_ = 0; // Number of recent lines hidden below the current viewport.

        // Multiline input state
        bool is_continuation_ = false; // True while Parser says the submitted text is syntactically incomplete.
        std::string continuation_buffer_; // Previously submitted lines for the current incomplete command.

        // History navigation
        int history_index_ = -1; // Current history position; -1 means the user is not browsing stored commands.
        std::string saved_input_before_browse_; // Draft input restored when the user navigates past newest history.

        // IPC descriptors
        int to_tab_fd_ = -1; // GUI write end connected to the shell worker's input read end.
        int from_tab_fd_ = -1; // GUI read end connected to the worker's output write end.

        // Thread / Process handles
        pthread_t thread_handle_ = 0; // Worker identifier used with pthread_join in thread mode.
        pid_t tab_pid_ = -1; // Worker process identifier used with waitpid/kill in process mode.
        bool is_closed_ = false; // Lifecycle guard that prevents duplicate termination work.
        bool is_executing_ = false; // GUI status indicating a command is in progress.

        ShellEngine* shell_ = nullptr; // Non-owning pointer valid while the worker's local ShellEngine exists.
    };

} // namespace Terminator

#endif // TERMINATOR_TAB_HPP closes the header guard.

// EOF
