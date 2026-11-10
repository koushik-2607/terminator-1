#ifndef TERMINATOR_AUTOCOMPLETE_HPP // Prevent this declaration file from being included more than once per translation unit.
#define TERMINATOR_AUTOCOMPLETE_HPP // Define the guard symbol for the remainder of this compilation.

#include "common.hpp" // Shares path and string helpers used by the completion implementation.
#include <string>     // Provides the std::string type used for typed prefixes and candidates.
#include <vector>     // Provides ordered lists of possible completion results.

namespace Terminator {

    /**
     * Autocomplete provides bash-style tab completion for commands and file paths.
     * Features:
     * - Executable command completion (from PATH and builtins) when typing the first token.
     * - File/directory completion when typing subsequent arguments or paths.
     * - Calculates longest common prefix across matches for partial auto-expansion.
     */
    class Autocomplete { // Groups completion operations as static utilities; no object state is required.
    public:
        struct CompletionResult { // Bundles the text to insert with all possible choices for the GUI.
            std::string prefix;                     // Shared prefix, or the full filename when one candidate matches.
            std::vector<std::string> candidates;    // Sorted matching names that may be displayed for user selection.
            bool is_unique = false;                 // True only when the GUI can safely insert the sole candidate.
        };

        /**
         * Perform completion on current input line up to cursor position.
         * @param line Full input line currently typed.
         * @param cursor_pos Cursor position within line.
         */
        static CompletionResult complete(const std::string& line, size_t cursor_pos, const std::string& cwd = "."); // Find matches before cursor_pos relative to the tab's cwd.

        /**
         * Find longest common prefix string among a list of candidate strings.
         */
        static std::string findLongestCommonPrefix(const std::vector<std::string>& matches); // Return the common leading substring for ambiguous candidates.

    private:
        static void completeCommands(const std::string& prefix, std::vector<std::string>& matches); // Helper for built-in/PATH command names; retained for command completion support.
        static void completeFiles(const std::string& prefix, std::vector<std::string>& matches, const std::string& cwd); // Helper that enumerates files and directories.
    };

} // namespace Terminator

#endif // TERMINATOR_AUTOCOMPLETE_HPP closes the one-inclusion guard.

// EOF
