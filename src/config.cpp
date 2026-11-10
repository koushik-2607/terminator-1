#include "config.hpp" // Declares ConfigManager and the key/action types used below.
#include <fstream>    // Reads and creates the user's plain-text hotkey file.
#include <iostream>   // Sends invalid-binding warnings to standard error.
#include <algorithm>  // Provides transform for case-insensitive parsing.
#include <cctype>     // Provides tolower for normalizing config spelling.

namespace Terminator {

    ConfigManager::ConfigManager() { // Construct defaults first so missing or bad user settings have a fallback.
        // Initialize with required default keybindings
        setupDefaults(); // Populate the built-in keymaps before reading optional overrides.

        const char* home = getenv("HOME"); // HOME is provided by the process environment and can be absent.
        if (home) { // Prefer a per-user dotfile when the environment supplies a home directory.
            config_path_ = std::string(home) + "/.terminatorrc"; // Store the complete path for later file operations and warnings.
        } else {
            config_path_ = ".terminatorrc"; // Fall back to a file in the current directory.
        }
    }

    void ConfigManager::setupDefaults() {
        key_to_action_.clear(); // Remove previous bindings so a reset cannot leave stale lookup entries.
        action_to_key_.clear(); // Clear the reverse map at the same time to keep the two views consistent.

        // Task 7 Default Specifications:
        // <- / -> : Move within the line
        // Ctrl+A / Ctrl+E : Move to start / end of line
        // Up / Down : Browse command history
        // Ctrl+U / Ctrl+K : Delete to start / end of line
        // Ctrl+W : Delete previous word
        // Ctrl+L : Clear screen
        // Ctrl+D : Exit the shell (close tab) / EOF
        // Ctrl+Shift+C / Ctrl+Shift+V : Copy / paste

        auto bind = [this](KeySym sym, unsigned int mod, TerminalAction act) { // Local helper captures this manager and records one binding in both maps.
            KeyCombo kc{sym, mod}; // Build the composite key that includes both the X11 symbol and modifier bits.
            key_to_action_[kc] = act; // Configure event-to-action lookup used by getAction().
            action_to_key_[act] = kc; // Configure the reverse mapping for the same action.
        };

        bind(XK_Left, 0, TerminalAction::MOVE_LEFT); // Left arrow moves the edit cursor without changing text.
        bind(XK_Right, 0, TerminalAction::MOVE_RIGHT); // Right arrow moves the edit cursor right.
        bind(XK_a, ControlMask, TerminalAction::START_OF_LINE); // Ctrl+A jumps to the start of the input buffer.
        bind(XK_A, ControlMask, TerminalAction::START_OF_LINE); // Accept the alternate uppercase X11 symbol too.
        bind(XK_e, ControlMask, TerminalAction::END_OF_LINE); // Ctrl+E jumps to the end of the current line.
        bind(XK_E, ControlMask, TerminalAction::END_OF_LINE); // Uppercase symbol variant for the same shortcut.
        bind(XK_Up, 0, TerminalAction::HISTORY_UP); // Up arrow recalls an older shared command.
        bind(XK_Down, 0, TerminalAction::HISTORY_DOWN); // Down arrow moves toward newer history or the saved draft.
        bind(XK_u, ControlMask, TerminalAction::DELETE_TO_START); // Ctrl+U removes text before the cursor.
        bind(XK_U, ControlMask, TerminalAction::DELETE_TO_START); // Uppercase symbol variant.
        bind(XK_k, ControlMask, TerminalAction::DELETE_TO_END); // Ctrl+K removes text after the cursor.
        bind(XK_K, ControlMask, TerminalAction::DELETE_TO_END); // Uppercase symbol variant.
        bind(XK_w, ControlMask, TerminalAction::DELETE_PREV_WORD); // Ctrl+W removes the preceding word.
        bind(XK_W, ControlMask, TerminalAction::DELETE_PREV_WORD); // Uppercase symbol variant.
        bind(XK_l, ControlMask, TerminalAction::CLEAR_SCREEN); // Ctrl+L clears visible terminal scrollback.
        bind(XK_L, ControlMask, TerminalAction::CLEAR_SCREEN); // Uppercase symbol variant.
        bind(XK_d, ControlMask, TerminalAction::EXIT_TAB); // Ctrl+D closes an empty tab or deletes at the cursor.
        bind(XK_D, ControlMask, TerminalAction::EXIT_TAB); // Uppercase symbol variant.
        bind(XK_c, ControlMask | ShiftMask, TerminalAction::COPY); // Ctrl+Shift+C copies the current input to X11 selection storage.
        bind(XK_C, ControlMask | ShiftMask, TerminalAction::COPY); // Uppercase symbol variant.
        bind(XK_v, ControlMask | ShiftMask, TerminalAction::PASTE); // Ctrl+Shift+V requests text from the X11 clipboard owner.
        bind(XK_V, ControlMask | ShiftMask, TerminalAction::PASTE); // Uppercase symbol variant.

        // Tab management shortcuts
        bind(XK_t, ControlMask | ShiftMask, TerminalAction::NEW_TAB); // Ctrl+Shift+T creates another tab.
        bind(XK_T, ControlMask | ShiftMask, TerminalAction::NEW_TAB); // Uppercase symbol variant.
        bind(XK_w, ControlMask | ShiftMask, TerminalAction::CLOSE_TAB); // Ctrl+Shift+W closes the selected tab.
        bind(XK_W, ControlMask | ShiftMask, TerminalAction::CLOSE_TAB); // Uppercase symbol variant.
        bind(XK_Tab, ControlMask, TerminalAction::NEXT_TAB); // Ctrl+Tab selects the following tab.
        bind(XK_Tab, ControlMask | ShiftMask, TerminalAction::PREV_TAB); // Ctrl+Shift+Tab selects the previous tab.
        bind(XK_ISO_Left_Tab, ControlMask, TerminalAction::PREV_TAB); // Some keyboards report reverse Tab using ISO_Left_Tab.
        bind(XK_Page_Down, ControlMask, TerminalAction::NEXT_TAB); // Ctrl+PageDown is an alternate next-tab shortcut.
        bind(XK_Page_Up, ControlMask, TerminalAction::PREV_TAB); // Ctrl+PageUp is an alternate previous-tab shortcut.
    }

    void ConfigManager::writeDefaultConfigFile(const std::string& filepath) {
        std::ofstream outfile(filepath); // Open/create the destination using the standard output-file stream.
        if (!outfile.is_open()) return; // If permissions or the directory prevent writing, keep built-in defaults.

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
        std::ifstream infile(config_path_); // Attempt to read the current user's bindings file.
        if (!infile.is_open()) { // A missing file is normal on first run.
            // Write default config file if not present
            writeDefaultConfigFile(config_path_); // Give the user an editable example containing the default shortcuts.
            return; // The in-memory defaults are already active, so no parsing is needed.
        }

        // Temporary maps to detect conflicts before committing changes
        std::unordered_map<KeyCombo, TerminalAction, KeyComboHash> temp_key_to_act; // Stage parsed bindings so validation happens before updating live mappings.
        std::unordered_map<TerminalAction, KeyCombo> temp_act_to_key; // Temporary reverse map kept in sync with the staged forward map.

        std::string line; // getline reuses this storage for each config line.
        int line_no = 0; // Track physical line numbers so warnings identify the problem location.

        while (std::getline(infile, line)) { // Read config records until EOF or a stream error.
            line_no++; // Increment before parsing so the first file line is reported as line one.
            line = trim(line); // Ignore surrounding whitespace while preserving the key's internal spelling.
            if (line.empty() || line[0] == '#') continue; // Blank and comment lines do not define bindings.

            size_t eq_pos = line.find('='); // The format separates action name from key combo with an equals sign.
            if (eq_pos == std::string::npos) continue; // Silently ignore malformed lines that have no assignment separator.

            std::string action_str = trim(line.substr(0, eq_pos)); // Extract and normalize the left-hand action field.
            std::string key_str = trim(line.substr(eq_pos + 1)); // Extract the key combination after the separator.

            TerminalAction action = parseActionName(action_str); // Convert a supported action name into the enum.
            if (action == TerminalAction::NONE) { // NONE signals that parsing found no recognized action.
                std::cerr << "Config Warning [" << config_path_ << ":" << line_no 
                          << "]: Unknown action '" << action_str << "'\n";
                continue;
            }

            KeyCombo combo; // Output object filled by parseKeyCombo when the textual key is valid.
            if (!parseKeyCombo(key_str, combo)) { // Invalid spelling or modifier prevents this binding from being used.
                std::cerr << "Config Warning [" << config_path_ << ":" << line_no 
                          << "]: Invalid key combination '" << key_str << "'\n";
                continue;
            }

            // Check for key conflicts
            if (temp_key_to_act.find(combo) != temp_key_to_act.end()) { // A key cannot invoke two distinct actions in this configuration.
                std::cerr << "Config Conflict [" << config_path_ << ":" << line_no 
                          << "]: Key combination '" << key_str 
                          << "' already assigned to '" 
                          << actionToString(temp_key_to_act[combo]) << "'. Falling back to defaults.\n";
                continue;
            }

            temp_key_to_act[combo] = action; // Stage the forward association after validation succeeds.
            temp_act_to_key[action] = combo; // Stage its reverse association as well.

            // Also support lowercase/uppercase variant for letters
            if (combo.keysym >= XK_A && combo.keysym <= XK_Z) { // X11 uses separate key symbols for alphabetic cases.
                KeyCombo lowerCombo = combo; // Copy all modifiers while changing only the letter case.
                lowerCombo.keysym = combo.keysym + (XK_a - XK_A); // Offset between uppercase and lowercase X11 symbols.
                temp_key_to_act[lowerCombo] = action; // Make either case invoke this same action.
            } else if (combo.keysym >= XK_a && combo.keysym <= XK_z) { // Also handle configs that spell the key lowercase.
                KeyCombo upperCombo = combo; // Copy the combo so modifiers remain unchanged.
                upperCombo.keysym = combo.keysym - (XK_a - XK_A); // Convert its symbol to uppercase by the same fixed offset.
                temp_key_to_act[upperCombo] = action; // Add the uppercase variant to event lookup.
            }
        }

        // If configuration loaded without fatal errors, apply valid bindings
        if (!temp_key_to_act.empty()) { // Preserve built-in defaults if the file supplied no valid bindings.
            for (const auto& pair : temp_key_to_act) { // Merge each validated custom key into the live map.
                key_to_action_[pair.first] = pair.second; // Later custom settings override the same default key.
                action_to_key_[pair.second] = pair.first; // Keep the reverse lookup synchronized with the applied binding.
            }
        }
    }

    TerminalAction ConfigManager::getAction(KeySym keysym, unsigned int state) const {
        // Strip out LockMask (Caps Lock), NumLock, etc. Keep only Ctrl, Shift, Mod1 (Alt)
        unsigned int clean_state = state & (ControlMask | ShiftMask | Mod1Mask); // Ignore Caps Lock and other state bits that do not define configured shortcuts.

        KeyCombo combo{keysym, clean_state}; // Match on the key plus only the meaningful modifier bits.
        auto it = key_to_action_.find(combo); // Hash lookup avoids scanning every configured binding.
        if (it != key_to_action_.end()) { // A present iterator represents a configured shortcut.
            return it->second; // Return the action the GUI should perform.
        }
        return TerminalAction::NONE; // Unknown key combinations are left for normal text-input handling.
    }

    bool ConfigManager::parseKeyCombo(const std::string& str, KeyCombo& out) {
        std::vector<std::string> parts = split(str, '+'); // Split modifier names and final key, for example Ctrl+Shift+T.
        if (parts.empty()) return false; // No token means there is no key to parse.

        unsigned int mods = 0; // Accumulate the X11 modifier mask from tokens before the final key.
        KeySym sym = 0; // Zero is used as the invalid/unset sentinel until a key name is recognized.

        for (size_t i = 0; i < parts.size(); ++i) { // Parse each plus-separated token in order.
            std::string part = trim(parts[i]); // Remove whitespace around this modifier or key name.
            std::string part_lower = part; // Keep the original spelling for case-sensitive single-letter conversion below.
            std::transform(part_lower.begin(), part_lower.end(), part_lower.begin(), ::tolower); // Normalize names like CTRL and Left.

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
                    if (sym == NoSymbol) return false; // XStringToKeysym could not map the textual key name to an X11 symbol.
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
                    return false; // Reject unknown modifier names instead of silently binding the wrong key.
                }
            }
        }

        if (sym == 0) return false; // Every valid combo must have a recognized nonzero KeySym.

        out.keysym = sym; // Store the parsed X11 key code in the caller's output structure.
        out.modifiers = mods; // Store the combined Control/Shift/Alt bitmask beside it.
        return true; // Report that both the key and all modifiers parsed successfully.
    }

    TerminalAction ConfigManager::parseActionName(const std::string& name) {
        std::string n = trim(name); // Copy before normalization so the caller's original string remains unchanged.
        std::transform(n.begin(), n.end(), n.begin(), ::tolower); // Make action-name matching case-insensitive.

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

        return TerminalAction::NONE; // Unknown action names are rejected by loadConfig().
    }

    std::string ConfigManager::actionToString(TerminalAction action) {
        switch (action) { // Map every enum value to its canonical config spelling.
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
            default: return "none"; // Provide a printable fallback for NONE or an unexpected enum value.
        }
    }

} // namespace Terminator
// EOF
