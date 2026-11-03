#include "config.hpp"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cctype>

namespace Terminator {

    ConfigManager::ConfigManager() {
        // Initialize with required default keybindings
        setupDefaults();

        const char* home = getenv("HOME");
        if (home) {
            config_path_ = std::string(home) + "/.terminatorrc";
        } else {
            config_path_ = ".terminatorrc";
        }
    }

    void ConfigManager::setupDefaults() {
        key_to_action_.clear();
        action_to_key_.clear();

        // Task 7 Default Specifications:
        // <- / -> : Move within the line
        // Ctrl+A / Ctrl+E : Move to start / end of line
        // Up / Down : Browse command history
        // Ctrl+U / Ctrl+K : Delete to start / end of line
        // Ctrl+W : Delete previous word
        // Ctrl+L : Clear screen
        // Ctrl+D : Exit the shell (close tab) / EOF
        // Ctrl+Shift+C / Ctrl+Shift+V : Copy / paste

        auto bind = [this](KeySym sym, unsigned int mod, TerminalAction act) {
            KeyCombo kc{sym, mod};
            key_to_action_[kc] = act;
            action_to_key_[act] = kc;
        };

        bind(XK_Left, 0, TerminalAction::MOVE_LEFT);
        bind(XK_Right, 0, TerminalAction::MOVE_RIGHT);
        bind(XK_a, ControlMask, TerminalAction::START_OF_LINE);
        bind(XK_A, ControlMask, TerminalAction::START_OF_LINE);
        bind(XK_e, ControlMask, TerminalAction::END_OF_LINE);
        bind(XK_E, ControlMask, TerminalAction::END_OF_LINE);
        bind(XK_Up, 0, TerminalAction::HISTORY_UP);
        bind(XK_Down, 0, TerminalAction::HISTORY_DOWN);
        bind(XK_u, ControlMask, TerminalAction::DELETE_TO_START);
        bind(XK_U, ControlMask, TerminalAction::DELETE_TO_START);
        bind(XK_k, ControlMask, TerminalAction::DELETE_TO_END);
        bind(XK_K, ControlMask, TerminalAction::DELETE_TO_END);
        bind(XK_w, ControlMask, TerminalAction::DELETE_PREV_WORD);
        bind(XK_W, ControlMask, TerminalAction::DELETE_PREV_WORD);
        bind(XK_l, ControlMask, TerminalAction::CLEAR_SCREEN);
        bind(XK_L, ControlMask, TerminalAction::CLEAR_SCREEN);
        bind(XK_d, ControlMask, TerminalAction::EXIT_TAB);
        bind(XK_D, ControlMask, TerminalAction::EXIT_TAB);
        bind(XK_c, ControlMask | ShiftMask, TerminalAction::COPY);
        bind(XK_C, ControlMask | ShiftMask, TerminalAction::COPY);
        bind(XK_v, ControlMask | ShiftMask, TerminalAction::PASTE);
        bind(XK_V, ControlMask | ShiftMask, TerminalAction::PASTE);

        // Tab management shortcuts
        bind(XK_t, ControlMask | ShiftMask, TerminalAction::NEW_TAB);
        bind(XK_T, ControlMask | ShiftMask, TerminalAction::NEW_TAB);
        bind(XK_w, ControlMask | ShiftMask, TerminalAction::CLOSE_TAB);
        bind(XK_W, ControlMask | ShiftMask, TerminalAction::CLOSE_TAB);
        bind(XK_Tab, ControlMask, TerminalAction::NEXT_TAB);
        bind(XK_Tab, ControlMask | ShiftMask, TerminalAction::PREV_TAB);
        bind(XK_ISO_Left_Tab, ControlMask, TerminalAction::PREV_TAB);
        bind(XK_Page_Down, ControlMask, TerminalAction::NEXT_TAB);
        bind(XK_Page_Up, ControlMask, TerminalAction::PREV_TAB);
    }

    void ConfigManager::writeDefaultConfigFile(const std::string& filepath) {
        std::ofstream outfile(filepath);
        if (!outfile.is_open()) return;

        outfile << "# ~/.terminatorrc - Custom hotkey configuration for Terminator\n";
        outfile << "# Format: action_name = Key_Combination\n";
        outfile << "#\n";
        outfile << "# Actions:\n";
        outfile << "#   move_left, move_right, line_start, line_end,\n";
        outfile << "#   history_prev, history_next, delete_to_start, delete_to_end,\n";
        outfile << "#   delete_prev_word, clear_screen, exit_tab, copy, paste,\n";
        outfile << "#   new_tab, close_tab, next_tab, prev_tab\n";
        outfile << "# Modifiers: Ctrl, Shift, Alt\n";
        outfile << "# Keys: A-Z, Left, Right, Up, Down, BackSpace, Tab, etc.\n\n";

        outfile << "move_left = Left\n";
        outfile << "move_right = Right\n";
        outfile << "line_start = Ctrl+A\n";
        outfile << "line_end = Ctrl+E\n";
        outfile << "history_prev = Up\n";
        outfile << "history_next = Down\n";
        outfile << "delete_to_start = Ctrl+U\n";
        outfile << "delete_to_end = Ctrl+K\n";
        outfile << "delete_prev_word = Ctrl+W\n";
        outfile << "clear_screen = Ctrl+L\n";
        outfile << "exit_tab = Ctrl+D\n";
        outfile << "copy = Ctrl+Shift+C\n";
        outfile << "paste = Ctrl+Shift+V\n";
        outfile << "new_tab = Ctrl+Shift+T\n";
        outfile << "close_tab = Ctrl+Shift+W\n";
        outfile << "next_tab = Ctrl+Tab\n";
        outfile << "prev_tab = Ctrl+Shift+Tab\n";
    }

    void ConfigManager::loadConfig() {
        std::ifstream infile(config_path_);
        if (!infile.is_open()) {
            // Write default config file if not present
            writeDefaultConfigFile(config_path_);
            return;
        }

        // Temporary maps to detect conflicts before committing changes
        std::unordered_map<KeyCombo, TerminalAction, KeyComboHash> temp_key_to_act;
        std::unordered_map<TerminalAction, KeyCombo> temp_act_to_key;

        std::string line;
        int line_no = 0;

        while (std::getline(infile, line)) {
            line_no++;
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;

            size_t eq_pos = line.find('=');
            if (eq_pos == std::string::npos) continue;

            std::string action_str = trim(line.substr(0, eq_pos));
            std::string key_str = trim(line.substr(eq_pos + 1));

            TerminalAction action = parseActionName(action_str);
            if (action == TerminalAction::NONE) {
                std::cerr << "Config Warning [" << config_path_ << ":" << line_no 
                          << "]: Unknown action '" << action_str << "'\n";
                continue;
            }

            KeyCombo combo;
            if (!parseKeyCombo(key_str, combo)) {
                std::cerr << "Config Warning [" << config_path_ << ":" << line_no 
                          << "]: Invalid key combination '" << key_str << "'\n";
                continue;
            }

            // Check for key conflicts
            if (temp_key_to_act.find(combo) != temp_key_to_act.end()) {
                std::cerr << "Config Conflict [" << config_path_ << ":" << line_no 
                          << "]: Key combination '" << key_str 
                          << "' already assigned to '" 
                          << actionToString(temp_key_to_act[combo]) << "'. Falling back to defaults.\n";
                continue;
            }

            temp_key_to_act[combo] = action;
            temp_act_to_key[action] = combo;

            // Also support lowercase/uppercase variant for letters
            if (combo.keysym >= XK_A && combo.keysym <= XK_Z) {
                KeyCombo lowerCombo = combo;
                lowerCombo.keysym = combo.keysym + (XK_a - XK_A);
                temp_key_to_act[lowerCombo] = action;
            } else if (combo.keysym >= XK_a && combo.keysym <= XK_z) {
                KeyCombo upperCombo = combo;
                upperCombo.keysym = combo.keysym - (XK_a - XK_A);
                temp_key_to_act[upperCombo] = action;
            }
        }

        // If configuration loaded without fatal errors, apply valid bindings
        if (!temp_key_to_act.empty()) {
            for (const auto& pair : temp_key_to_act) {
                key_to_action_[pair.first] = pair.second;
                action_to_key_[pair.second] = pair.first;
            }
        }
    }

    TerminalAction ConfigManager::getAction(KeySym keysym, unsigned int state) const {
        // Strip out LockMask (Caps Lock), NumLock, etc. Keep only Ctrl, Shift, Mod1 (Alt)
        unsigned int clean_state = state & (ControlMask | ShiftMask | Mod1Mask);

        KeyCombo combo{keysym, clean_state};
        auto it = key_to_action_.find(combo);
        if (it != key_to_action_.end()) {
            return it->second;
        }
        return TerminalAction::NONE;
    }

    bool ConfigManager::parseKeyCombo(const std::string& str, KeyCombo& out) {
        std::vector<std::string> parts = split(str, '+');
        if (parts.empty()) return false;

        unsigned int mods = 0;
        KeySym sym = 0;

        for (size_t i = 0; i < parts.size(); ++i) {
            std::string part = trim(parts[i]);
            std::string part_lower = part;
            std::transform(part_lower.begin(), part_lower.end(), part_lower.begin(), ::tolower);

            if (i == parts.size() - 1) {
                // The last element is the key name
                if (part_lower == "left") sym = XK_Left;
                else if (part_lower == "right") sym = XK_Right;
                else if (part_lower == "up") sym = XK_Up;
                else if (part_lower == "down") sym = XK_Down;
                else if (part_lower == "backspace") sym = XK_BackSpace;
                else if (part_lower == "delete") sym = XK_Delete;
                else if (part_lower == "tab") sym = XK_Tab;
                else if (part_lower == "return" || part_lower == "enter") sym = XK_Return;
                else if (part.length() == 1) {
                    char c = part[0];
                    if (c >= 'A' && c <= 'Z') sym = XK_A + (c - 'A');
                    else if (c >= 'a' && c <= 'z') sym = XK_a + (c - 'a');
                    else if (c >= '0' && c <= '9') sym = XK_0 + (c - '0');
                    else sym = c;
                } else {
                    sym = XStringToKeysym(part.c_str());
                    if (sym == NoSymbol) return false;
                }
            } else {
                // Modifiers
                if (part_lower == "ctrl" || part_lower == "control") {
                    mods |= ControlMask;
                } else if (part_lower == "shift") {
                    mods |= ShiftMask;
                } else if (part_lower == "alt" || part_lower == "mod1") {
                    mods |= Mod1Mask;
                } else {
                    return false;
                }
            }
        }

        if (sym == 0) return false;

        out.keysym = sym;
        out.modifiers = mods;
        return true;
    }

    TerminalAction ConfigManager::parseActionName(const std::string& name) {
        std::string n = trim(name);
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);

        if (n == "move_left") return TerminalAction::MOVE_LEFT;
        if (n == "move_right") return TerminalAction::MOVE_RIGHT;
        if (n == "line_start") return TerminalAction::START_OF_LINE;
        if (n == "line_end") return TerminalAction::END_OF_LINE;
        if (n == "history_prev" || n == "history_up") return TerminalAction::HISTORY_UP;
        if (n == "history_next" || n == "history_down") return TerminalAction::HISTORY_DOWN;
        if (n == "delete_to_start") return TerminalAction::DELETE_TO_START;
        if (n == "delete_to_end") return TerminalAction::DELETE_TO_END;
        if (n == "delete_prev_word") return TerminalAction::DELETE_PREV_WORD;
        if (n == "clear_screen") return TerminalAction::CLEAR_SCREEN;
        if (n == "exit_tab") return TerminalAction::EXIT_TAB;
        if (n == "copy") return TerminalAction::COPY;
        if (n == "paste") return TerminalAction::PASTE;
        if (n == "new_tab") return TerminalAction::NEW_TAB;
        if (n == "close_tab") return TerminalAction::CLOSE_TAB;
        if (n == "next_tab") return TerminalAction::NEXT_TAB;
        if (n == "prev_tab") return TerminalAction::PREV_TAB;

        return TerminalAction::NONE;
    }

    std::string ConfigManager::actionToString(TerminalAction action) {
        switch (action) {
            case TerminalAction::MOVE_LEFT: return "move_left";
            case TerminalAction::MOVE_RIGHT: return "move_right";
            case TerminalAction::START_OF_LINE: return "line_start";
            case TerminalAction::END_OF_LINE: return "line_end";
            case TerminalAction::HISTORY_UP: return "history_prev";
            case TerminalAction::HISTORY_DOWN: return "history_next";
            case TerminalAction::DELETE_TO_START: return "delete_to_start";
            case TerminalAction::DELETE_TO_END: return "delete_to_end";
            case TerminalAction::DELETE_PREV_WORD: return "delete_prev_word";
            case TerminalAction::CLEAR_SCREEN: return "clear_screen";
            case TerminalAction::EXIT_TAB: return "exit_tab";
            case TerminalAction::COPY: return "copy";
            case TerminalAction::PASTE: return "paste";
            case TerminalAction::NEW_TAB: return "new_tab";
            case TerminalAction::CLOSE_TAB: return "close_tab";
            case TerminalAction::NEXT_TAB: return "next_tab";
            case TerminalAction::PREV_TAB: return "prev_tab";
            default: return "none";
        }
    }

} // namespace Terminator
