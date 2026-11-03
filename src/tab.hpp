#ifndef TERMINATOR_TAB_HPP
#define TERMINATOR_TAB_HPP

#include "common.hpp"
#include "shell.hpp"
#include "history.hpp"
#include <string>
#include <vector>
#include <pthread.h>

namespace Terminator {

    /**
     * Tab encapsulates all state and communications for a single terminal tab.
     * Manages:
     * - Line buffer for terminal output and scrolling
     * - Active input buffer, cursor position, multiline continuation
     * - IPC pipes between GUI and Tab Shell
     * - Thread / Process lifecycle depending on execution mode
     */
    class Tab {
    public:
        Tab(int id, bool use_process_mode, HistoryManager* history_mgr);
        ~Tab();

        // Prevent copying
        Tab(const Tab&) = delete;
        Tab& operator=(const Tab&) = delete;

        // Terminal text buffer operations
        void appendText(const std::string& text);
        void appendLine(const std::string& line);
        void clearScreen();
        void scroll(int delta_lines, int visible_rows);

        // Input & line editing operations
        void insertChar(char c);
        void backspace();
        void deleteChar();
        void moveCursorLeft();
        void moveCursorRight();
        void moveCursorStart();
        void moveCursorEnd();
        void deleteToStart();
        void deleteToEnd();
        void deletePrevWord();

        // History browsing
        void historyPrev();
        void historyNext();

        // Tab lifecycle & signals
        void sendCommand(const std::string& cmd);
        void sendSignal(int sig);
        void terminate();

        // Accessors
        int getId() const { return id_; }
        std::string getTitle() const { return title_; }
        void setTitle(const std::string& title) { title_ = title; }
        const std::vector<std::string>& getLines() const { return lines_; }
        const std::string& getCurrentInput() const { return current_input_; }
        size_t getCursorPos() const { return cursor_pos_; }
        int getScrollOffset() const { return scroll_offset_; }
        bool isContinuation() const { return is_continuation_; }
        std::string getContinuationBuffer() const { return continuation_buffer_; }
        void setContinuation(bool cont, const std::string& buf) {
            is_continuation_ = cont;
            continuation_buffer_ = buf;
        }
        void resetInput() {
            current_input_.clear();
            cursor_pos_ = 0;
            history_index_ = -1;
            saved_input_before_browse_.clear();
        }

        int getReadFd() const { return from_tab_fd_; }
        bool isClosed() const { return is_closed_; }
        void markClosed() { is_closed_ = true; }
        bool isExecuting() const { return is_executing_; }
        void setExecuting(bool val) { is_executing_ = val; }

        pid_t getPid() const { return tab_pid_; }
        std::string getPrompt() const;

    private:
        void startThreadMode();
        void startProcessMode();
        static void* threadEntry(void* arg);
        void runShellLoop(int in_fd, int out_fd);

        int id_;
        bool use_process_mode_;
        std::string title_;
        HistoryManager* history_;

        // Screen buffer
        std::vector<std::string> lines_;
        std::string current_input_;
        size_t cursor_pos_ = 0;
        int scroll_offset_ = 0;

        // Multiline input state
        bool is_continuation_ = false;
        std::string continuation_buffer_;

        // History navigation
        int history_index_ = -1;
        std::string saved_input_before_browse_;

        // IPC descriptors
        int to_tab_fd_ = -1;
        int from_tab_fd_ = -1;

        // Thread / Process handles
        pthread_t thread_handle_ = 0;
        pid_t tab_pid_ = -1;
        bool is_closed_ = false;
        bool is_executing_ = false;

        ShellEngine* shell_ = nullptr;
    };

} // namespace Terminator

#endif // TERMINATOR_TAB_HPP

