#ifndef TERMINATOR_GUI_HPP
#define TERMINATOR_GUI_HPP

#include "common.hpp"
#include "tab.hpp"
#include "config.hpp"
#include "history.hpp"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <vector>
#include <memory>
#include <string>

namespace Terminator {

    /**
     * TabHeaderRegion stores clickable bounding boxes for tabs in the tab bar.
     */
    struct TabHeaderRegion {
        int tab_index;      // Index in tabs_ array (-1 for new tab '+' button)
        int x;
        int y;
        int width;
        int height;
        bool is_close_btn;  // True if click is on 'x' close button
    };

    /**
     * TerminalGUI handles:
     * - X11 display connection, window creation, GC, and font loading
     * - Multi-tab rendering, tab switching, creation, and destruction
     * - Non-busy-wait event loop using poll() on X11 connection and tab pipes
     * - Keyboard input routing, hotkeys, and bash-style line editing
     * - X11 clipboard copy & paste (XA_PRIMARY & CLIPBOARD atoms)
     */
    class TerminalGUI {
    public:
        TerminalGUI(bool use_process_mode);
        ~TerminalGUI();

        /**
         * Initialize X11 window, fonts, graphics context, and hotkey configurations.
         */
        bool init();

        /**
         * Run the non-busy-wait main event loop.
         */
        void run();

        /**
         * Create a new terminal tab.
         */
        void createTab();

        /**
         * Close tab at index.
         */
        void closeTab(size_t index);

        /**
         * Switch active tab to index.
         */
        void selectTab(size_t index);

    private:
        // Event handlers
        void handleXEvent(XEvent& event);
        void handleKeyPress(XKeyEvent& event);
        void handleButtonPress(XButtonEvent& event);
        void handleSelectionRequest(XSelectionRequestEvent& event);
        void handleSelectionNotify(XSelectionEvent& event);
        void handleConfigure(XConfigureEvent& event);

        // Rendering functions
        void redraw();
        void drawTabBar();
        void drawTerminalScreen();
        void drawCursor(int x, int y);

        // Tab pipe I/O drain
        void drainTabPipes();

        // Color allocation
        unsigned long getColor(const std::string& hex_rgb);

        bool use_process_mode_;
        bool running_ = true;

        // X11 handles
        Display* display_ = nullptr;
        Window window_ = 0;
        GC gc_ = 0;
        XFontStruct* font_ = nullptr;
        int screen_ = 0;
        int char_width_ = 8;
        int char_height_ = 16;
        int char_ascent_ = 12;

        int window_width_ = DEFAULT_WINDOW_WIDTH;
        int window_height_ = DEFAULT_WINDOW_HEIGHT;

        // Color pixel values
        unsigned long col_bg_ = 0;
        unsigned long col_fg_ = 0;
        unsigned long col_tab_bar_ = 0;
        unsigned long col_tab_active_ = 0;
        unsigned long col_tab_inactive_ = 0;
        unsigned long col_tab_border_ = 0;
        unsigned long col_prompt_ = 0;
        unsigned long col_cursor_ = 0;

        // Clipboard
        std::string clipboard_buffer_;
        Atom atom_clipboard_ = 0;
        Atom atom_targets_ = 0;
        Atom atom_utf8_string_ = 0;

        // Subsystems
        std::unique_ptr<HistoryManager> history_mgr_;
        ConfigManager config_mgr_;

        // Active tabs
        std::vector<std::unique_ptr<Tab>> tabs_;
        size_t active_tab_index_ = 0;
        int next_tab_id_ = 1;

        std::vector<TabHeaderRegion> tab_regions_;
    };

} // namespace Terminator

#endif // TERMINATOR_GUI_HPP

