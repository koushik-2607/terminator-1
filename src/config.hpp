#ifndef TERMINATOR_CONFIG_HPP
#define TERMINATOR_CONFIG_HPP

#include "common.hpp"
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace Terminator {

    /**
     * Actions triggered by keyboard shortcuts in Terminator.
     */
    enum class TerminalAction {
        NONE,
        MOVE_LEFT,
        MOVE_RIGHT,
        START_OF_LINE,
        END_OF_LINE,
        HISTORY_UP,
        HISTORY_DOWN,
        DELETE_TO_START,
        DELETE_TO_END,
        DELETE_PREV_WORD,
        CLEAR_SCREEN,
        EXIT_TAB,
        COPY,
        PASTE,
        NEW_TAB,
        CLOSE_TAB,
        NEXT_TAB,
        PREV_TAB
    };

    /**
     * KeyCombo encapsulates an X11 KeySym and modifier bitmask (Control, Shift, Alt).
     */
    struct KeyCombo {
        KeySym keysym = 0;
        unsigned int modifiers = 0; // ControlMask, ShiftMask, Mod1Mask (Alt)

        bool operator==(const KeyCombo& other) const {
            return keysym == other.keysym && modifiers == other.modifiers;
        }
    };

    struct KeyComboHash {
        std::size_t operator()(const KeyCombo& k) const {
            return std::hash<unsigned long>()(k.keysym) ^ (std::hash<unsigned int>()(k.modifiers) << 1);
        }
    };

    /**
     * ConfigManager loads, validates, and manages hotkey bindings from ~/.terminatorrc.
     * Guarantees conflict detection and graceful fallback to defaults.
     */
    class ConfigManager {
    public:
        ConfigManager();

        /**
         * Load configuration from ~/.terminatorrc (or create default if not found).
         */
        void loadConfig();

        /**
         * Resolve an incoming X11 KeyPress event to a TerminalAction.
         * @param keysym The X11 KeySym from XLookupKeysym / XLookupString.
         * @param state The event modifier bitmask (event.xkey.state).
         */
        TerminalAction getAction(KeySym keysym, unsigned int state) const;

        /**
         * Convert a string representation of key combo to KeyCombo struct.
         */
        static bool parseKeyCombo(const std::string& str, KeyCombo& out);

        /**
         * Convert a string action name to TerminalAction enum.
         */
        static TerminalAction parseActionName(const std::string& name);

        /**
         * String name for a TerminalAction.
         */
        static std::string actionToString(TerminalAction action);

    private:
        // Set up the default bash-style hotkeys required by Task 7
        void setupDefaults();

        // Write a well-documented default ~/.terminatorrc file
        void writeDefaultConfigFile(const std::string& filepath);

        std::unordered_map<KeyCombo, TerminalAction, KeyComboHash> key_to_action_;
        std::unordered_map<TerminalAction, KeyCombo> action_to_key_;
        std::string config_path_;
    };

} // namespace Terminator

#endif // TERMINATOR_CONFIG_HPP

