#ifndef TERMINATOR_CONFIG_HPP // Prevent repeated declarations when GUI and tests both include this header.
#define TERMINATOR_CONFIG_HPP // Mark the configuration interface as included.

#include "common.hpp"       // Provides shared string helpers and standard POSIX declarations.
#include <X11/Xlib.h>       // Defines KeySym and modifier masks for X11 keyboard events.
#include <X11/keysym.h>     // Defines symbolic names such as XK_Left and XK_Tab.
#include <string>           // Stores config paths and textual action/key names.
#include <unordered_map>    // Maps key combinations to actions with average constant-time lookup.
#include <vector>           // Supplies temporary lists while parsing key strings.

namespace Terminator {

    /**
     * Actions triggered by keyboard shortcuts in Terminator.
     */
    enum class TerminalAction { // Strongly typed list of operations the GUI may run for a keypress.
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
    struct KeyCombo { // Represents a key plus the modifier keys held at the same time.
        KeySym keysym = 0; // X11's numeric symbol for the physical/logical key, such as XK_a or XK_Left.
        unsigned int modifiers = 0; // Bitwise combination of ControlMask, ShiftMask, and Mod1Mask (Alt).

        bool operator==(const KeyCombo& other) const { // Equality is needed when the combo is used as a hash-map key.
            return keysym == other.keysym && modifiers == other.modifiers; // Both key and modifier bits must match.
        }
    };

    struct KeyComboHash { // Supplies unordered_map with a stable hash for KeyCombo values.
        std::size_t operator()(const KeyCombo& k) const { // Called by unordered_map when it locates a combo bucket.
            return std::hash<unsigned long>()(k.keysym) ^ (std::hash<unsigned int>()(k.modifiers) << 1); // Combine hashes of key and modifiers.
        }
    };

    /**
     * ConfigManager loads, validates, and manages hotkey bindings from ~/.terminatorrc.
     * Guarantees conflict detection and graceful fallback to defaults.
     */
    class ConfigManager { // Owns default and user-configured keyboard bindings for the GUI.
    public:
        ConfigManager(); // Builds defaults and determines the user's config-file path.

        /**
         * Load configuration from ~/.terminatorrc (or create default if not found).
         */
        void loadConfig(); // Read ~/.terminatorrc, validating each action and key before applying it.

        /**
         * Resolve an incoming X11 KeyPress event to a TerminalAction.
         * @param keysym The X11 KeySym from XLookupKeysym / XLookupString.
         * @param state The event modifier bitmask (event.xkey.state).
         */
        TerminalAction getAction(KeySym keysym, unsigned int state) const; // Translate an X11 event's key and modifier mask into an action.

        /**
         * Convert a string representation of key combo to KeyCombo struct.
         */
        static bool parseKeyCombo(const std::string& str, KeyCombo& out); // Parse text such as Ctrl+Shift+T into an X11 key and modifier mask.

        /**
         * Convert a string action name to TerminalAction enum.
         */
        static TerminalAction parseActionName(const std::string& name); // Convert config text like history_prev to the enum value.

        /**
         * String name for a TerminalAction.
         */
        static std::string actionToString(TerminalAction action); // Convert an enum back to the canonical config spelling.

    private:
        // Set up the default bash-style hotkeys required by Task 7
        void setupDefaults(); // Populate the required editing, clipboard, and tab-navigation shortcuts.

        // Write a well-documented default ~/.terminatorrc file
        void writeDefaultConfigFile(const std::string& filepath); // Create an example config file when no user file exists.

        std::unordered_map<KeyCombo, TerminalAction, KeyComboHash> key_to_action_; // Fast lookup direction used for incoming key events.
        std::unordered_map<TerminalAction, KeyCombo> action_to_key_; // Reverse mapping useful for displaying or editing bindings.
        std::string config_path_; // Absolute or relative path to this user's Terminator configuration.
    };

} // namespace Terminator

#endif // TERMINATOR_CONFIG_HPP closes the header guard.

// EOF
