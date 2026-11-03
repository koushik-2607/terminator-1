#include "gui.hpp"
#include "autocomplete.hpp"
#include "parser.hpp"
#include <iostream>
#include <poll.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>
#include <csignal>

namespace Terminator {

    TerminalGUI::TerminalGUI(bool use_process_mode)
        : use_process_mode_(use_process_mode) {
    }

    TerminalGUI::~TerminalGUI() {
        // Clean up tabs
        tabs_.clear();

        if (display_) {
            if (gc_) XFreeGC(display_, gc_);
            if (font_) XFreeFont(display_, font_);
            if (window_) XDestroyWindow(display_, window_);
            XCloseDisplay(display_);
        }
    }


    unsigned long TerminalGUI::getColor(const std::string& hex_rgb) {
        if (!display_) return 0;
        Colormap cmap = DefaultColormap(display_, screen_);
        XColor color;
        if (XParseColor(display_, cmap, hex_rgb.c_str(), &color) &&
            XAllocColor(display_, cmap, &color)) {
            return color.pixel;
        }
        return WhitePixel(display_, screen_);
    }

    bool TerminalGUI::init() {
        // GUI process ignores SIGINT and SIGTSTP so Terminator itself never stops
        signal(SIGINT, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);


        //nullptr represents the display name [default is nullptr]
        display_ = XOpenDisplay(nullptr);
        if (!display_) {
            std::cerr << "Terminator Error: Unable to open X11 display (is DISPLAY set?)\n";
            return false;
        }

        screen_ = DefaultScreen(display_);
        Window root = RootWindow(display_, screen_);

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
        window_ = XCreateSimpleWindow(
            display_, root,
            100, 100, window_width_, window_height_,
            1, col_tab_border_, col_bg_
        );

        // Window properties
        XStoreName(display_, window_, "Terminator");
        XSelectInput(display_, window_,
            ExposureMask | KeyPressMask | ButtonPressMask |
            StructureNotifyMask | PropertyChangeMask
        );

        // Load monospace font
        font_ = XLoadQueryFont(display_, "9x15");
        if (!font_) {
            font_ = XLoadQueryFont(display_, "fixed");
        }

        if (font_) {
            char_width_  = font_->max_bounds.width;
            char_ascent_ = font_->ascent;
            char_height_ = font_->ascent + font_->descent;
        }

        gc_ = XCreateGC(display_, window_, 0, nullptr);
        if (font_) {
            XSetFont(display_, gc_, font_->fid);
        }

        // Initialize Atoms for clipboard copy/paste
        atom_clipboard_   = XInternAtom(display_, "CLIPBOARD", False);
        atom_targets_     = XInternAtom(display_, "TARGETS", False);
        atom_utf8_string_ = XInternAtom(display_, "UTF8_STRING", False);

        // Load hotkeys configuration from ~/.terminatorrc
        config_mgr_.loadConfig();

        // Initialize shared history manager (parent creates POSIX shared memory)
        history_mgr_ = std::make_unique<HistoryManager>(true);

        // Show window
        XMapWindow(display_, window_);
        XFlush(display_);

        // Create the initial tab
        createTab();

        return true;
    }

    void TerminalGUI::createTab() {
        if (tabs_.size() >= 16) return;

        auto tab = std::make_unique<Tab>(next_tab_id_++, use_process_mode_, history_mgr_.get());
        tabs_.push_back(std::move(tab));
        active_tab_index_ = tabs_.size() - 1;
        redraw();
    }

    void TerminalGUI::closeTab(size_t index) {
        if (index >= tabs_.size()) return;

        tabs_[index]->terminate();
        tabs_.erase(tabs_.begin() + index);

        if (tabs_.empty()) {
            running_ = false;
            return;
        }

        if (active_tab_index_ >= tabs_.size()) {
            active_tab_index_ = tabs_.size() - 1;
        }

        redraw();
    }

    void TerminalGUI::selectTab(size_t index) {
        if (index < tabs_.size()) {
            active_tab_index_ = index;
            redraw();
        }
    }

    void TerminalGUI::drainTabPipes() {
        bool updated = false;

        for (size_t i = 0; i < tabs_.size(); ++i) {
            Tab* tab = tabs_[i].get();
            int fd = tab->getReadFd();
            if (fd < 0) continue;

            char buf[4096];
            std::string incoming;
            ssize_t n;
            bool eof_reached = false;

            while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
                buf[n] = '\0';
                incoming.append(buf, n);
            }

            if (n == 0) {
                eof_reached = true;
            }

            if (!incoming.empty()) {
                // Parse OSC title escape sequence: \033]0;<title>\007
                size_t osc_pos;
                while ((osc_pos = incoming.find(OSC_TITLE_PREFIX)) != std::string::npos) {
                    size_t osc_end = incoming.find(OSC_TITLE_SUFFIX, osc_pos);
                    if (osc_end != std::string::npos) {
                        std::string title = incoming.substr(osc_pos + OSC_TITLE_PREFIX.length(), 
                                                            osc_end - (osc_pos + OSC_TITLE_PREFIX.length()));
                        tab->setTitle(title);
                        incoming.erase(osc_pos, (osc_end + OSC_TITLE_SUFFIX.length()) - osc_pos);
                    } else {
                        break;
                    }
                }

                // Parse exit notification
                if (incoming.find("\033]EXIT\007") != std::string::npos) {
                    closeTab(i);
                    return;
                }

                // Parse prompt notification
                size_t p_pos;
                while ((p_pos = incoming.find("\033]PROMPT\007")) != std::string::npos) {
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

    void TerminalGUI::run() {
        int x11_fd = ConnectionNumber(display_);

        while (running_) {
            // Prepare pollfd set containing X11 socket and all active tab read pipes
            std::vector<struct pollfd> pfds;

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
            int ret = poll(pfds.data(), pfds.size(), 50);

            if (ret > 0) {
                // Drain any output from tab shells
                drainTabPipes();

                // Process all pending X11 events
                while (XPending(display_)) {
                    XEvent event;
                    XNextEvent(display_, &event);
                    handleXEvent(event);
                }
            } else if (ret == 0) {
                // Timeout check: drain any background updates
                drainTabPipes();
            }
        }
    }

    void TerminalGUI::handleXEvent(XEvent& event) {
        switch (event.type) {
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

    void TerminalGUI::handleConfigure(XConfigureEvent& event) {
        if (event.width != window_width_ || event.height != window_height_) {
            window_width_ = event.width;
            window_height_ = event.height;
            redraw();
        }
    }

    void TerminalGUI::handleKeyPress(XKeyEvent& event) {
        if (tabs_.empty()) return;
        Tab* tab = tabs_[active_tab_index_].get();

        char text[64];
        KeySym keysym = 0;
        int len = XLookupString(&event, text, sizeof(text) - 1, &keysym, nullptr);
        if (len >= 0) text[len] = '\0';

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
            auto res = Autocomplete::complete(line, tab->getCursorPos());

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
                for (const auto& c : res.candidates) choices += c + "  ";
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
        std::string input_before_cursor = input.substr(0, tab->getCursorPos());
        int cursor_x = 8 + prompt_width + XTextWidth(font_, input_before_cursor.c_str(), input_before_cursor.length());
        int cursor_y = draw_y - char_ascent_;

        // Draw cursor block
        drawCursor(cursor_x, cursor_y);

        // If cursor is over a character in the input string, draw that character in col_bg_
        // on top of the cyan cursor block for high contrast visibility
        if (tab->getCursorPos() < input.length()) {
            char ch = input[tab->getCursorPos()];
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
