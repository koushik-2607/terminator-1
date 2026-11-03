#ifndef TERMINATOR_AUTOCOMPLETE_HPP
#define TERMINATOR_AUTOCOMPLETE_HPP

#include "common.hpp"
#include <string>
#include <vector>

namespace Terminator {

    /**
     * Autocomplete provides bash-style tab completion for commands and file paths.
     * Features:
     * - Executable command completion (from PATH and builtins) when typing the first token.
     * - File/directory completion when typing subsequent arguments or paths.
     * - Calculates longest common prefix across matches for partial auto-expansion.
     */
    class Autocomplete {
    public:
        struct CompletionResult {
            std::string prefix;                     // Common prefix among all candidates
            std::vector<std::string> candidates;    // All matching choices
            bool is_unique = false;                 // Exactly 1 match found
        };

        /**
         * Perform completion on current input line up to cursor position.
         * @param line Full input line currently typed.
         * @param cursor_pos Cursor position within line.
         */
        static CompletionResult complete(const std::string& line, size_t cursor_pos);

        /**
         * Find longest common prefix string among a list of candidate strings.
         */
        static std::string findLongestCommonPrefix(const std::vector<std::string>& matches);

    private:
        static void completeCommands(const std::string& prefix, std::vector<std::string>& matches);
        static void completeFiles(const std::string& prefix, std::vector<std::string>& matches);
    };

} // namespace Terminator

#endif // TERMINATOR_AUTOCOMPLETE_HPP

