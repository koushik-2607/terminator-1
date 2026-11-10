#ifndef TERMINATOR_GUI_HPP // Prevent duplicate class definitions when this interface is included from several modules.
#define TERMINATOR_GUI_HPP // Define the unique guard for the GUI header.

#include "common.hpp" // Supplies constants, path helpers, and shared POSIX declarations.
#include "tab.hpp" // Gives the GUI each tab's input buffer, process connection, and display data.
#include "config.hpp" // Provides configurable key-to-action mappings for X11 events.
#include "history.hpp" // Exposes shared command history used by history navigation and Ctrl+R search.
#include <X11/Xlib.h> // Declares X11 display, window, graphics context, and event types.
#include <X11/Xutil.h> // Declares XLookupString and font/text measurement helpers.
#include <X11/keysym.h> // Defines symbolic key constants used in keyboard dispatch.
#include <X11/Xatom.h> // Defines standard X11 clipboard/selection atom constants.
#include <vector> // Stores tabs and clickable tab-header regions.
#include <memory> // unique_ptr owns tabs and the shared HistoryManager safely.
#include <string> // Stores clipboard, search, and textual display content.

namespace Terminator {

    /**
     * TabHeaderRegion stores clickable bounding boxes for tabs in the tab bar.
     */
    struct TabHeaderRegion { // Screen rectangle and action metadata for one clickable tab-bar item.
        int tab_index;      // Index in tabs_; -1 identifies the '+' button instead of a real tab.
        int x; // Left edge in X window pixel coordinates.
        int y; // Top edge in X window pixel coordinates.
        int width; // Horizontal size used for hit testing mouse clicks.
        int height; // Vertical size used for hit testing mouse clicks.
        bool is_close_btn;  // True when this rectangle represents the tab's close control.
    };

    /**
     * TerminalGUI handles:
     * - X11 display connection, window creation, GC, and font loading
     * - Multi-tab rendering, tab switching, creation, and destruction
     * - Non-busy-wait event loop using poll() on X11 connection and tab pipes
     * - Keyboard input routing, hotkeys, and bash-style line editing
     * - X11 clipboard copy & paste (XA_PRIMARY & CLIPBOARD atoms)
     */
    class TerminalGUI { // Owns the X11 front end and routes input/output between the user and tab shells.
    public:
        TerminalGUI(bool use_process_mode); // Remember whether new tabs should use process workers or thread workers.
        ~TerminalGUI(); // Release tabs and X11 resources in reverse ownership order.

        /**
         * Initialize X11 window, fonts, graphics context, and hotkey configurations.
         */
        bool init(); // Open the display, create drawing resources, and initialize shared state; false means startup failed.

        /**
         * Run the non-busy-wait main event loop.
         */
        void run(); // Block in the poll-based event loop until the window closes.

        /**
         * Create a new terminal tab.
         */
        void createTab(); // Construct a shell tab and add it to the owned tab collection.

        /**
         * Close tab at index.
         */
        void closeTab(size_t index); // Stop and remove the indexed tab, then repair the active-tab selection.

        /**
         * Switch active tab to index.
         */
        void selectTab(size_t index); // Make an existing tab the visible recipient of keyboard input.

    private:
        // Event handlers
        void handleXEvent(XEvent& event); // Dispatch the X server's tagged event union to a specialized handler.
        void handleKeyPress(XKeyEvent& event); // Translate key symbols into editing, shell, clipboard, or tab actions.
        void handleButtonPress(XButtonEvent& event); // Hit-test mouse coordinates against tab/header rectangles.
        void handleSelectionRequest(XSelectionRequestEvent& event); // Supply clipboard bytes when another X client requests them.
        void handleSelectionNotify(XSelectionEvent& event); // Receive clipboard text requested from another X selection owner.
        void handleConfigure(XConfigureEvent& event); // Update drawing dimensions when the window is resized.

        // Rendering functions
        void redraw(); // Clear and repaint the complete visible window from current GUI/tab state.
        void drawTabBar(); // Draw tab labels and record their clickable hit regions.
        void drawTerminalScreen(); // Draw scrollback lines, the active prompt/input, and caret.
        void drawCursor(int x, int y); // Draw the block cursor at pixel position x,y.

        // Tab pipe I/O drain
        void drainTabPipes(); // Read available shell output from tab pipes and append it to each tab's screen buffer.

        // Color allocation
        unsigned long getColor(const std::string& hex_rgb); // Convert a #RRGGBB color string into an allocated X11 pixel value.

        bool use_process_mode_; // Execution architecture passed to every newly constructed Tab.
        bool running_ = true; // Event-loop condition; window-close events set it false.

        // X11 handles
        Display* display_ = nullptr; // Xlib connection to the X server; null before init succeeds.
        Window window_ = 0; // XID identifying the app window on the connected display.
        GC gc_ = 0; // Graphics context holds drawing state such as font and foreground color.
        XFontStruct* font_ = nullptr; // Loaded core X font used for drawing and measuring text.
        int screen_ = 0; // Default screen index selected from the X display connection.
        int char_width_ = 8; // Fallback fixed-font cell width in pixels.
        int char_height_ = 16; // Fallback fixed-font cell height in pixels.
        int char_ascent_ = 12; // Fallback baseline-to-top distance used to align text.

        int window_width_ = DEFAULT_WINDOW_WIDTH; // Current client width, updated by ConfigureNotify.
        int window_height_ = DEFAULT_WINDOW_HEIGHT; // Current client height, updated by ConfigureNotify.

        // Color pixel values
        unsigned long col_bg_ = 0; // X server pixel for the terminal background.
        unsigned long col_fg_ = 0; // X server pixel for normal foreground text.
        unsigned long col_tab_bar_ = 0; // Background pixel for the tab strip.
        unsigned long col_tab_active_ = 0; // Highlight pixel for the selected tab.
        unsigned long col_tab_inactive_ = 0; // Background pixel for unselected tabs.
        unsigned long col_tab_border_ = 0; // Border pixel used around tabs and controls.
        unsigned long col_prompt_ = 0; // Pixel color used to distinguish prompts from output.
        unsigned long col_cursor_ = 0; // Fill color used for the block caret.

        // Clipboard
        std::string clipboard_buffer_; // Locally owned copied text returned through SelectionRequest.
        Atom atom_clipboard_ = 0; // X atom naming the CLIPBOARD selection convention.
        Atom atom_targets_ = 0; // X atom used to negotiate supported clipboard formats.
        Atom atom_utf8_string_ = 0; // X atom naming UTF8_STRING clipboard contents.

        // Subsystems
        std::unique_ptr<HistoryManager> history_mgr_; // Owns the one shared history mapping used by every tab.
        ConfigManager config_mgr_; // Stores and resolves user-configured hotkeys.

        // Active tabs
        std::vector<std::unique_ptr<Tab>> tabs_; // Owns each live tab object and its worker resources.
        size_t active_tab_index_ = 0; // Index of the tab currently rendered and receiving input.
        int next_tab_id_ = 1; // Monotonically assigned human-facing tab identifier.

        std::vector<TabHeaderRegion> tab_regions_; // Hit-test geometry rebuilt when drawing the tab bar.
        bool history_search_active_ = false; // Selects the Ctrl+R term-entry mode instead of normal command input.
        std::string history_search_term_; // Text typed after the history-search prompt.
        std::vector<std::string> completion_choices_; // Pending numbered filename candidates after ambiguous Tab completion.
    };

} // namespace Terminator

#endif // TERMINATOR_GUI_HPP closes the one-inclusion guard.

// EOF
