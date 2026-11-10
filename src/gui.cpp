#include "gui.hpp" // Declares the TerminalGUI controller and its X11-owned state.
#include "autocomplete.hpp" // Provides filename candidates when the user presses Tab.
#include "parser.hpp" // Detects incomplete multiline commands before execution.
#include <iostream> // Writes startup and display diagnostics to standard error.
#include <poll.h> // Waits on the X server socket and tab output pipes without busy-waiting.
#include <unistd.h> // Provides read and close for the IPC pipe descriptors.
#include <cstring> // Provides byte/string utilities for clipboard and pipe data.
#include <algorithm> // Provides min/max and sorting helpers used by drawing/input logic.
#include <csignal> // Provides SIGINT and SIGTSTP constants forwarded to shell processes.

namespace Terminator {

    TerminalGUI::TerminalGUI(bool use_process_mode) // Construct controller state before connecting to X11.
        : use_process_mode_(use_process_mode) { // Save the worker architecture for later Tab creation.
    }

    TerminalGUI::~TerminalGUI() {
        // Clean up tabs
        tabs_.clear(); // Stop/destroy workers while GUI-owned IPC and shared-history state are still alive.

        if (display_) { // X resources are valid only if XOpenDisplay succeeded.
            if (gc_) XFreeGC(display_, gc_); // Release the graphics context allocated by XCreateGC.
            if (font_) XFreeFont(display_, font_); // Release the server-side core font.
            if (window_) XDestroyWindow(display_, window_); // Ask the X server to destroy the top-level window.
            XCloseDisplay(display_); // Close the client/server connection after releasing dependent resources.
        }
    }


    unsigned long TerminalGUI::getColor(const std::string& hex_rgb) {
        if (!display_) return 0; // No connected display means no screen colormap is available.
        Colormap cmap = DefaultColormap(display_, screen_); // Obtain the selected screen's color allocation table.
        XColor color; // Xlib fills this structure from the textual color request.
        if (XParseColor(display_, cmap, hex_rgb.c_str(), &color) && // Parse text such as #RRGGBB into XColor components.
            XAllocColor(display_, cmap, &color)) { // Ask the X server to allocate a pixel value for those components.
            return color.pixel; // X drawing functions consume a pixel ID, not the original color string.
        }
        return WhitePixel(display_, screen_);
    }

    bool TerminalGUI::init() { // Create the display connection, window resources, shared history, and first tab.
        // GUI process ignores SIGINT and SIGTSTP so Terminator itself never stops
        signal(SIGINT, SIG_IGN); // Keep terminal Ctrl+C from terminating the GUI; input handling signals child groups instead.
        signal(SIGTSTP, SIG_IGN); // Keep Ctrl+Z from suspending the GUI process itself.


        //nullptr represents the display name [default is nullptr]
        display_ = XOpenDisplay(nullptr); // nullptr asks Xlib to connect to the server named by DISPLAY.
        if (!display_) { // A null result means the X server connection failed.
            std::cerr << "Terminator Error: Unable to open X11 display (is DISPLAY set?)\n";
            return false;
        }

        screen_ = DefaultScreen(display_); // Select the X server's default screen.
        Window root = RootWindow(display_, screen_); // Obtain the root window that owns top-level windows.

        // Allocate clean, high-contrast terminal colors
        col_bg_           = getColor("#1e1e1e"); // Deep dark terminal background
        col_fg_           = getColor("#d4d4d4"); // Clear readable light gray text
        col_tab_bar_      = getColor("#252526"); // Tab bar dark header
        col_tab_active_   = getColor("#3c3836"); // Active tab highlight
        col_tab_inactive_ = getColor("#282828"); // Inactive tab
        col_tab_border_   = getColor("#504945"); // Tab divider border
        col_prompt_       = getColor("#50fa7b"); // Bright vibrant green prompt
        col_cursor_       = getColor("#00bcd4"); // Cyan block cursor

        // Create application window
        window_ = XCreateSimpleWindow( // Request a basic window resource from the X server.
            display_, root,
            100, 100, window_width_, window_height_,
            1, col_tab_border_, col_bg_
        );

        // Window properties
        XStoreName(display_, window_, "Terminator"); // Set the title displayed by the window manager.
        XSelectInput(display_, window_, // Subscribe to the event classes used by this GUI.
            ExposureMask | KeyPressMask | ButtonPressMask |
            StructureNotifyMask | PropertyChangeMask
        );

        // Load monospace font
        font_ = XLoadQueryFont(display_, "9x15"); // Prefer a readable fixed-width font suitable for terminal columns.
        if (!font_) { // Minimal X servers may not install this optional font.
            font_ = XLoadQueryFont(display_, "fixed"); // Fall back to X's standard fixed font.
        }

        if (font_) {
            char_width_  = font_->max_bounds.width; // Cache cell width for cursor placement and layout.
            char_ascent_ = font_->ascent; // Cache baseline-to-top distance for XDrawString coordinates.
            char_height_ = font_->ascent + font_->descent; // A glyph cell spans both ascent and descent.
        }

        gc_ = XCreateGC(display_, window_, 0, nullptr); // Create reusable X drawing state attached to the window.
        if (font_) {
            XSetFont(display_, gc_, font_->fid); // Store the selected font in the graphics context used for drawing.
        }

        // Initialize Atoms for clipboard copy/paste
        atom_clipboard_   = XInternAtom(display_, "CLIPBOARD", False);
        atom_targets_     = XInternAtom(display_, "TARGETS", False);
        atom_utf8_string_ = XInternAtom(display_, "UTF8_STRING", False);

        // Load hotkeys configuration from ~/.terminatorrc
        config_mgr_.loadConfig();

        // Initialize shared history manager (parent creates POSIX shared memory)
        history_mgr_ = std::make_unique<HistoryManager>(true); // GUI creates/owns one history mapping shared by all tab workers.

        // Show window
        XMapWindow(display_, window_);
        XFlush(display_);

        // Create the initial tab
        createTab(); // Start with one shell tab so the first render has an active input target.

        return true; // Signal to main() that startup succeeded and run() may begin polling events.
    }

    void TerminalGUI::createTab() {
        if (tabs_.size() >= 16) return; // Limit worker/resource count and avoid overflowing the tab bar.

        auto tab = std::make_unique<Tab>(next_tab_id_++, use_process_mode_, history_mgr_.get()); // Create a shell worker and pass shared history as a non-owning pointer.
        tabs_.push_back(std::move(tab)); // Transfer ownership into the vector so the GUI controls tab lifetime.
        active_tab_index_ = tabs_.size() - 1; // Focus the newly created tab.
        redraw();
    }

    void TerminalGUI::closeTab(size_t index) {
        if (index >= tabs_.size()) return; // Reject stale mouse/key indices instead of indexing beyond the vector.

        tabs_[index]->terminate();
        tabs_.erase(tabs_.begin() + index); // Destroy the tab and invoke its worker/descriptor cleanup.

        if (tabs_.empty()) { // Closing the last tab ends the application window.
            running_ = false;
            return; // Do not attempt to select a tab when none remain.
        }

        if (active_tab_index_ >= tabs_.size()) { // Removing a tab before/at the active one can invalidate its index.
            active_tab_index_ = tabs_.size() - 1; // Select the new last tab as a valid fallback.
        }

        redraw();
    }

    void TerminalGUI::selectTab(size_t index) {
        if (index < tabs_.size()) { // Validate the requested index before selecting it.
            active_tab_index_ = index; // Route subsequent input and rendering to this tab.
            redraw();
        }
    }

    void TerminalGUI::drainTabPipes() { // Pull available worker output into tab buffers after poll signals readable pipe data.
        bool updated = false; // Redraw only when output, titles, or tab lifecycle changed.

        for (size_t i = 0; i < tabs_.size(); ++i) { // Process all tabs because inactive tabs continue running.
            Tab* tab = tabs_[i].get(); // Borrow the GUI-owned tab pointer for this iteration.
            int fd = tab->getReadFd(); // Read end of this tab worker's output pipe.
            if (fd < 0) continue; // A closed tab has no output endpoint.

            char buf[4096]; // Fixed read chunk avoids allocating based on untrusted child output size.
            std::string incoming; // Collect all bytes currently available before interpreting control sequences.
            ssize_t n; // Receives the byte count or EOF/error result from read().
            bool eof_reached = false;

            while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) { // Drain a nonblocking pipe until it would block or reaches EOF.
                buf[n] = '\0';
                incoming.append(buf, n);
            }

            if (n == 0) { // A zero-byte read is EOF: the worker closed every write endpoint.
                eof_reached = true;
            }

            if (!incoming.empty()) {
                // Parse OSC title escape sequence: \033]0;<title>\007
                size_t osc_pos;
                while ((osc_pos = incoming.find(OSC_TITLE_PREFIX)) != std::string::npos) { // Locate internal OSC title updates embedded in normal output.
                    size_t osc_end = incoming.find(OSC_TITLE_SUFFIX, osc_pos);
                    if (osc_end != std::string::npos) { // Do not consume an incomplete control sequence split across reads.
                        std::string title = incoming.substr(osc_pos + OSC_TITLE_PREFIX.length(), 
                                                            osc_end - (osc_pos + OSC_TITLE_PREFIX.length()));
                        tab->setTitle(title); // Update the tab bar label with the title sent by ShellEngine.
                        incoming.erase(osc_pos, (osc_end + OSC_TITLE_SUFFIX.length()) - osc_pos);
                    } else {
                        break;
                    }
                }

                // Parse exit notification
                if (incoming.find("\033]EXIT\007") != std::string::npos) { // Shell exit built-in sends this private marker to request tab closure.
                    closeTab(i);
                    return;
                }

                // Parse prompt notification
                size_t p_pos;
                while ((p_pos = incoming.find("\033]PROMPT\007")) != std::string::npos) { // Prompt marker signals that worker finished the current command.
                    tab->setExecuting(false);
                    incoming.erase(p_pos, 9);
                }

                tab->appendText(incoming);
                updated = true;
            }

            if (eof_reached) {
                // Tab child process or thread pipe closed -> tab has finished or crashed
                closeTab(i);
                return;
            }
        }

        if (updated) {
            redraw();
        }
    }

    void TerminalGUI::run() { // Main non-busy-wait loop; Xlib remains on this GUI thread only.
        int x11_fd = ConnectionNumber(display_); // Obtain the socket descriptor Xlib uses to communicate with the X server.

        while (running_) { // Continue until DestroyNotify/last-tab closure changes the lifecycle flag.
            // Prepare pollfd set containing X11 socket and all active tab read pipes
            std::vector<struct pollfd> pfds; // poll watches the X connection and every active tab output pipe together.

            // X11 Connection Socket
            struct pollfd x_pfd;
            x_pfd.fd = x11_fd;
            x_pfd.events = POLLIN;
            x_pfd.revents = 0;
            pfds.push_back(x_pfd);

            // Tab Output Pipes
            for (const auto& tab : tabs_) {
                int fd = tab->getReadFd();
                if (fd >= 0) {
                    struct pollfd t_pfd;
                    t_pfd.fd = fd;
                    t_pfd.events = POLLIN | POLLHUP;
                    t_pfd.revents = 0;
                    pfds.push_back(t_pfd);
                }
            }

            // Non-busy-wait polling (50ms interval allows smooth cursor and pipeline responsiveness)
            int ret = poll(pfds.data(), pfds.size(), 50); // Sleep for at most 50 ms, avoiding a CPU-burning busy loop.

            if (ret > 0) { // A positive result means one or more descriptors changed state.
                // Drain any output from tab shells
                drainTabPipes();

                // Process all pending X11 events
                while (XPending(display_)) { // XPending reports events already buffered by Xlib after the socket wakes.
                    XEvent event; // Tagged union filled with the next concrete X event.
                    XNextEvent(display_, &event); // Remove one event from Xlib's queue for dispatch.
                    handleXEvent(event);
                }
            } else if (ret == 0) {
                // Timeout check: drain any background updates
                drainTabPipes();
            }
        }
    }

    void TerminalGUI::handleXEvent(XEvent& event) { // Dispatch the common XEvent union using its event-type tag.
        switch (event.type) { // Read only the event-specific union member matching this type.
            case Expose:
                if (event.xexpose.count == 0) {
                    redraw();
                }
                break;

            case ConfigureNotify:
                handleConfigure(event.xconfigure);
                break;

            case KeyPress:
                handleKeyPress(event.xkey);
                break;

            case ButtonPress:
                handleButtonPress(event.xbutton);
                break;

            case SelectionRequest:
                handleSelectionRequest(event.xselectionrequest);
                break;

            case SelectionNotify:
                handleSelectionNotify(event.xselection);
                break;

            case DestroyNotify:
                running_ = false;
                break;
        }
    }

    void TerminalGUI::handleConfigure(XConfigureEvent& event) { // Respond to the server's notification that the window changed size.
        if (event.width != window_width_ || event.height != window_height_) {
            window_width_ = event.width;
            window_height_ = event.height;
            redraw();
        }
    }

    void TerminalGUI::handleKeyPress(XKeyEvent& event) { // Route one X keyboard event to editing, shell signal, clipboard, or tab behavior.
        if (tabs_.empty()) return; // There is no input target if every tab has closed.
        Tab* tab = tabs_[active_tab_index_].get();

        char text[64]; // Buffer for text bytes produced by the current key event.
        KeySym keysym = 0; // X11 symbolic key output, used to recognize arrows/control shortcuts.
        int len = XLookupString(&event, text, sizeof(text) - 1, &keysym, nullptr); // Translate hardware/event data to text and a KeySym.
        if (len >= 0) text[len] = '\0';

        if (history_search_active_) {
            if (keysym == XK_Return || keysym == XK_KP_Enter) {
                const auto matches = history_mgr_->search(history_search_term_); // Search a semaphore-protected history snapshot.
                if (matches.empty()) tab->appendLine("No match for search term in history");
                else for (const auto& match : matches) tab->appendLine(match);
                history_search_active_ = false; history_search_term_.clear(); redraw(); return;
            }
            if (keysym == XK_BackSpace) { if (!history_search_term_.empty()) history_search_term_.pop_back(); redraw(); return; }
            if (!(event.state & (ControlMask | Mod1Mask))) for (int i = 0; i < len; ++i) if ((unsigned char)text[i] >= 32) history_search_term_ += text[i];
            redraw(); return;
        }
        if ((event.state & ControlMask) && (keysym == XK_r || keysym == XK_R)) { // Ctrl+R enters the dedicated history-search text mode.
            history_search_active_ = true; history_search_term_.clear(); redraw(); return;
        }
        if (!completion_choices_.empty() && keysym >= XK_1 && keysym <= XK_9) {
            size_t chosen = keysym - XK_1;
            if (chosen < completion_choices_.size()) {
                std::string line = tab->getCurrentInput(); size_t p = tab->getCursorPos();
                while (p > 0 && !std::isspace(line[p-1])) --p;
                std::string updated = line.substr(0,p) + completion_choices_[chosen] + line.substr(tab->getCursorPos());
                tab->resetInput(); for (char c : updated) tab->insertChar(c);
            }
            completion_choices_.clear(); redraw(); return;
        }
        if (!completion_choices_.empty() && keysym != XK_Shift_L && keysym != XK_Shift_R) completion_choices_.clear();

        // Check for Alt+1..9 to switch tabs directly
        if ((event.state & Mod1Mask) && keysym >= XK_1 && keysym <= XK_9) {
            size_t target_idx = keysym - XK_1;
            if (target_idx < tabs_.size()) {
                selectTab(target_idx);
                return;
            }
        }

        // Check for Ctrl+Tab / Ctrl+Shift+Tab to cycle tabs
        if ((event.state & ControlMask) && (keysym == XK_Tab || keysym == XK_ISO_Left_Tab)) {
            if (!tabs_.empty()) {
                if (event.state & ShiftMask) {
                    selectTab((active_tab_index_ + tabs_.size() - 1) % tabs_.size());
                } else {
                    selectTab((active_tab_index_ + 1) % tabs_.size());
                }
                return;
            }
        }

        // Check for Ctrl+C
        if ((event.state & ControlMask) && (keysym == XK_c || keysym == XK_C) && !(event.state & ShiftMask)) {
            tab->appendText("^C\n");
            tab->sendSignal(SIGINT);
            tab->setContinuation(false, "");
            tab->resetInput();
            redraw();
            return;
        }

        // Check for Ctrl+Z
        if ((event.state & ControlMask) && (keysym == XK_z || keysym == XK_Z)) {
            tab->appendText("^Z\n");
            tab->sendSignal(SIGTSTP);
            tab->setContinuation(false, "");
            tab->resetInput();
            redraw();
            return;
        }

        // Check configurable hotkeys from ConfigManager
        TerminalAction action = config_mgr_.getAction(keysym, event.state);

        switch (action) {
            case TerminalAction::MOVE_LEFT:
                tab->moveCursorLeft();
                redraw();
                return;

            case TerminalAction::MOVE_RIGHT:
                tab->moveCursorRight();
                redraw();
                return;

            case TerminalAction::START_OF_LINE:
                tab->moveCursorStart();
                redraw();
                return;

            case TerminalAction::END_OF_LINE:
                tab->moveCursorEnd();
                redraw();
                return;

            case TerminalAction::HISTORY_UP:
                tab->historyPrev();
                redraw();
                return;

            case TerminalAction::HISTORY_DOWN:
                tab->historyNext();
                redraw();
                return;

            case TerminalAction::DELETE_TO_START:
                tab->deleteToStart();
                redraw();
                return;

            case TerminalAction::DELETE_TO_END:
                tab->deleteToEnd();
                redraw();
                return;

            case TerminalAction::DELETE_PREV_WORD:
                tab->deletePrevWord();
                redraw();
                return;

            case TerminalAction::CLEAR_SCREEN:
                tab->clearScreen();
                redraw();
                return;

            case TerminalAction::EXIT_TAB:
                if (tab->getCurrentInput().empty()) {
                    closeTab(active_tab_index_);
                    return;
                } else {
                    // In bash, Ctrl+D on non-empty line deletes character under cursor
                    tab->deleteChar();
                    redraw();
                    return;
                }
                break;

            case TerminalAction::COPY: {
                // Copy current input buffer or selection to X11 clipboard
                clipboard_buffer_ = tab->getCurrentInput();
                XSetSelectionOwner(display_, atom_clipboard_, window_, CurrentTime);
                XSetSelectionOwner(display_, XA_PRIMARY, window_, CurrentTime);
                return;
            }

            case TerminalAction::PASTE: {
                // Request clipboard data from owner
                XConvertSelection(display_, atom_clipboard_, atom_utf8_string_,
                                  atom_clipboard_, window_, CurrentTime);
                return;
            }

            case TerminalAction::NEW_TAB:
                createTab();
                return;

            case TerminalAction::CLOSE_TAB:
                closeTab(active_tab_index_);
                return;

            case TerminalAction::NEXT_TAB:
                if (!tabs_.empty()) {
                    selectTab((active_tab_index_ + 1) % tabs_.size());
                }
                return;

            case TerminalAction::PREV_TAB:
                if (!tabs_.empty()) {
                    selectTab((active_tab_index_ + tabs_.size() - 1) % tabs_.size());
                }
                return;

            default:
                break;
        }

        // Tab auto-completion
        if (keysym == XK_Tab) {
            std::string line = tab->getCurrentInput();
            auto res = Autocomplete::complete(line, tab->getCursorPos(), expandTilde(tab->getTitle()));

            if (res.is_unique && !res.prefix.empty()) {
                // Insert completed word
                size_t p = tab->getCursorPos();
                while (p > 0 && !std::isspace(line[p - 1])) p--;
                std::string new_input = line.substr(0, p) + res.prefix + line.substr(tab->getCursorPos());
                tab->resetInput();
                for (char c : new_input) tab->insertChar(c);
            } else if (res.candidates.size() > 1) {
                // Show completion candidates
                tab->appendLine(tab->getPrompt() + line);
                std::string choices;
                completion_choices_ = res.candidates;
                for (size_t i = 0; i < res.candidates.size(); ++i) choices += std::to_string(i + 1) + ". " + res.candidates[i] + "  ";
                tab->appendLine(choices);
                // Also expand common prefix if any
                if (!res.prefix.empty()) {
                    size_t p = tab->getCursorPos();
                    while (p > 0 && !std::isspace(line[p - 1])) p--;
                    std::string new_input = line.substr(0, p) + res.prefix + line.substr(tab->getCursorPos());
                    tab->resetInput();
                    for (char c : new_input) tab->insertChar(c);
                }
            }
            redraw();
            return;
        }

        // Return / Enter Key
        if (keysym == XK_Return || keysym == XK_KP_Enter) {
            std::string line = tab->getCurrentInput();
            std::string full_command;

            if (tab->isContinuation()) {
                full_command = tab->getContinuationBuffer() + "\n" + line;
            } else {
                full_command = line;
            }

            // Print entered line in terminal buffer
            tab->appendLine(tab->getPrompt() + line);

            // Check if input is incomplete (Task 3 multiline input)
            if (Parser::isInputIncomplete(full_command)) {
                tab->setContinuation(true, full_command);
                tab->resetInput();
            } else {
                // Input is complete: execute command
                tab->setContinuation(false, "");
                tab->resetInput();

                if (!full_command.empty()) {
                    // Record in shared history
                    history_mgr_->add(full_command);
                    tab->setExecuting(true);
                    tab->sendCommand(full_command);
                }
            }

            redraw();
            return;
        }

        // Backspace Key
        if (keysym == XK_BackSpace) {
            tab->backspace();
            redraw();
            return;
        }

        // Delete Key
        if (keysym == XK_Delete) {
            tab->deleteChar();
            redraw();
            return;
        }

        // Standard text typing
        if (!(event.state & ControlMask) && !(event.state & Mod1Mask)) {
            for (int i = 0; i < len; ++i) {
                unsigned char c = (unsigned char)text[i];
                if (c >= 32 && c <= 126) {
                    tab->insertChar(c);
                }
            }
            redraw();
        }
    }

    void TerminalGUI::handleButtonPress(XButtonEvent& event) {
        if (event.button == Button1) {
            // Left click: tab switching or new tab '+'
            for (const auto& reg : tab_regions_) {
                if (event.x >= reg.x && event.x < reg.x + reg.width &&
                    event.y >= reg.y && event.y < reg.y + reg.height) {
                    
                    if (reg.tab_index == -1) {
                        createTab();
                    } else {
                        selectTab(reg.tab_index);
                    }
                    return;
                }
            }
        } else if (event.button == Button2) {
            // Middle click: close tab
            for (const auto& reg : tab_regions_) {
                if (event.x >= reg.x && event.x < reg.x + reg.width &&
                    event.y >= reg.y && event.y < reg.y + reg.height) {
                    if (reg.tab_index >= 0) {
                        closeTab(reg.tab_index);
                        return;
                    }
                }
            }
        } else if (event.button == Button4) {
            // Mouse wheel scroll UP
            if (!tabs_.empty()) {
                int visible_rows = (window_height_ - TAB_BAR_HEIGHT) / char_height_;
                tabs_[active_tab_index_]->scroll(3, visible_rows);
                redraw();
            }
        } else if (event.button == Button5) {
            // Mouse wheel scroll DOWN
            if (!tabs_.empty()) {
                int visible_rows = (window_height_ - TAB_BAR_HEIGHT) / char_height_;
                tabs_[active_tab_index_]->scroll(-3, visible_rows);
                redraw();
            }
        }
    }

    void TerminalGUI::handleSelectionRequest(XSelectionRequestEvent& event) {
        XSelectionEvent reply;
        reply.type = SelectionNotify;
        reply.requestor = event.requestor;
        reply.selection = event.selection;
        reply.target = event.target;
        reply.time = event.time;
        reply.property = None;

        if (event.target == XA_STRING || event.target == atom_utf8_string_) {
            XChangeProperty(display_, event.requestor, event.property,
                            event.target, 8, PropModeReplace,
                            (const unsigned char*)clipboard_buffer_.c_str(),
                            clipboard_buffer_.length());
            reply.property = event.property;
        }

        XSendEvent(display_, event.requestor, False, 0, (XEvent*)&reply);
        XFlush(display_);
    }

    void TerminalGUI::handleSelectionNotify(XSelectionEvent& event) {
        if (event.property == None || tabs_.empty()) return;

        Atom actual_type;
        int actual_format;
        unsigned long nitems, bytes_after;
        unsigned char* prop_data = nullptr;

        if (XGetWindowProperty(display_, window_, event.property, 0, 4096, False,
                               AnyPropertyType, &actual_type, &actual_format,
                               &nitems, &bytes_after, &prop_data) == Success) {
            if (prop_data) {
                std::string pasted((char*)prop_data, nitems);
                Tab* tab = tabs_[active_tab_index_].get();
                for (char c : pasted) {
                    if (c >= 32 && c <= 126) {
                        tab->insertChar(c);
                    }
                }
                XFree(prop_data);
                redraw();
            }
        }
    }

    void TerminalGUI::drawTabBar() {
        tab_regions_.clear();

        // Fill tab bar background
        XSetForeground(display_, gc_, col_tab_bar_);
        XFillRectangle(display_, window_, gc_, 0, 0, window_width_, TAB_BAR_HEIGHT);

        // Draw bottom separator line
        XSetForeground(display_, gc_, col_tab_border_);
        XDrawLine(display_, window_, gc_, 0, TAB_BAR_HEIGHT - 1, window_width_, TAB_BAR_HEIGHT - 1);

        int current_x = 8;
        int padding_h = 10;

        for (size_t i = 0; i < tabs_.size(); ++i) {
            const auto& tab = tabs_[i];
            bool is_active = (i == active_tab_index_);

            // Format tab title matching PDF requirement: [ 1: ~/Downloads ] [ 2: /tmp ] [ + ]
            std::string label = "[ " + std::to_string(tab->getId()) + ": " + tab->getTitle() + " ]";
            int text_w = XTextWidth(font_, label.c_str(), label.length());
            int tab_w = text_w + padding_h * 2;

            // Draw tab background
            XSetForeground(display_, gc_, is_active ? col_tab_active_ : col_tab_inactive_);
            XFillRectangle(display_, window_, gc_, current_x, 3, tab_w, TAB_BAR_HEIGHT - 4);

            // Draw border
            XSetForeground(display_, gc_, col_tab_border_);
            XDrawRectangle(display_, window_, gc_, current_x, 3, tab_w, TAB_BAR_HEIGHT - 4);

            // Draw tab text
            XSetForeground(display_, gc_, is_active ? WhitePixel(display_, screen_) : col_fg_);
            XDrawString(display_, window_, gc_,
                        current_x + padding_h,
                        3 + char_ascent_ + 4,
                        label.c_str(), label.length());

            // Save region for mouse click
            TabHeaderRegion reg;
            reg.tab_index = (int)i;
            reg.x = current_x;
            reg.y = 3;
            reg.width = tab_w;
            reg.height = TAB_BAR_HEIGHT - 4;
            reg.is_close_btn = false;
            tab_regions_.push_back(reg);

            current_x += tab_w + 6;
        }

        // Draw new tab '+' button: [ + ]
        std::string new_tab_btn = "[ + ]";
        int btn_w = XTextWidth(font_, new_tab_btn.c_str(), new_tab_btn.length()) + 16;
        XSetForeground(display_, gc_, col_tab_inactive_);
        XFillRectangle(display_, window_, gc_, current_x, 3, btn_w, TAB_BAR_HEIGHT - 4);

        XSetForeground(display_, gc_, col_tab_border_);
        XDrawRectangle(display_, window_, gc_, current_x, 3, btn_w, TAB_BAR_HEIGHT - 4);

        XSetForeground(display_, gc_, col_prompt_);
        XDrawString(display_, window_, gc_,
                    current_x + 8,
                    3 + char_ascent_ + 4,
                    new_tab_btn.c_str(), new_tab_btn.length());

        TabHeaderRegion plus_reg;
        plus_reg.tab_index = -1; // Represents new tab
        plus_reg.x = current_x;
        plus_reg.y = 3;
        plus_reg.width = btn_w;
        plus_reg.height = TAB_BAR_HEIGHT - 4;
        plus_reg.is_close_btn = false;
        tab_regions_.push_back(plus_reg);
    }

    void TerminalGUI::drawTerminalScreen() {
        if (tabs_.empty()) return;
        Tab* tab = tabs_[active_tab_index_].get();

        int term_y_start = TAB_BAR_HEIGHT + 4;
        int usable_height = window_height_ - term_y_start - 6;
        int total_visible_rows = usable_height / char_height_;
        if (total_visible_rows < 1) total_visible_rows = 1;

        const auto& lines = tab->getLines();
        int scroll = tab->getScrollOffset();

        // Calculate rows allocated to history lines and active input line
        int history_rows = total_visible_rows - 1;
        if (history_rows < 0) history_rows = 0;

        int total_lines = (int)lines.size();
        int end_line_idx = total_lines - scroll;
        int start_line_idx = end_line_idx - history_rows;
        if (start_line_idx < 0) start_line_idx = 0;

        int draw_y = term_y_start + char_ascent_;

        // Render history lines
        XSetForeground(display_, gc_, col_fg_);
        for (int i = start_line_idx; i < end_line_idx && i < total_lines; ++i) {
            const std::string& l = lines[i];
            if (!l.empty()) {
                XDrawString(display_, window_, gc_, 8, draw_y, l.c_str(), l.length());
            }
            draw_y += char_height_;
        }

        // Render prompt and active input line
        std::string prompt = tab->getPrompt();
        std::string input = tab->getCurrentInput();
        if (history_search_active_) { prompt = "Enter search term: "; input = history_search_term_; }

        // Draw prompt in green
        XSetForeground(display_, gc_, col_prompt_);
        XDrawString(display_, window_, gc_, 8, draw_y, prompt.c_str(), prompt.length());

        int prompt_width = XTextWidth(font_, prompt.c_str(), prompt.length());

        // Draw input text in white/light-gray
        XSetForeground(display_, gc_, col_fg_);
        if (!input.empty()) {
            XDrawString(display_, window_, gc_, 8 + prompt_width, draw_y, input.c_str(), input.length());
        }

        // Calculate cursor position
        size_t cursor_pos = history_search_active_ ? input.size() : std::min(tab->getCursorPos(), input.size());
        std::string input_before_cursor = input.substr(0, cursor_pos);
        int cursor_x = 8 + prompt_width + XTextWidth(font_, input_before_cursor.c_str(), input_before_cursor.length());
        int cursor_y = draw_y - char_ascent_;

        // Draw cursor block
        drawCursor(cursor_x, cursor_y);

        // If cursor is over a character in the input string, draw that character in col_bg_
        // on top of the cyan cursor block for high contrast visibility
        if (cursor_pos < input.length()) {
            char ch = input[cursor_pos];
            XSetForeground(display_, gc_, col_bg_);
            XDrawString(display_, window_, gc_, cursor_x, draw_y, &ch, 1);
        }
    }

    void TerminalGUI::drawCursor(int x, int y) {
        XSetForeground(display_, gc_, col_cursor_);
        XFillRectangle(display_, window_, gc_, x, y, char_width_, char_height_);
    }

    void TerminalGUI::redraw() {
        if (!display_ || !window_) return;

        // Clear terminal window area
        XSetForeground(display_, gc_, col_bg_);
        XFillRectangle(display_, window_, gc_, 0, 0, window_width_, window_height_);

        // Render tab bar
        drawTabBar();

        // Render terminal output and input
        drawTerminalScreen();

        XFlush(display_);
    }

} // namespace Terminator
// EOF
