#ifndef TERMINATOR_PARSER_HPP
#define TERMINATOR_PARSER_HPP

#include "common.hpp"
#include <string>
#include <vector>
#include <functional>

namespace Terminator {

    /**
     * Single command within a pipeline, including its arguments and redirections.
     */
    struct Command {
        std::vector<std::string> args;      // Command binary and arguments
        std::string input_file;             // Redirection file for '<'
        std::string output_file;            // Redirection file for '>'
        bool append_output = false;         // '>>'
    };

    /**
     * Pipeline represents a series of commands connected by '|'.
     */
    struct Pipeline {
        std::vector<Command> commands;      // List of commands in pipeline
        bool run_in_background = false;     // True if trailing '&' was present
        std::string raw_command;            // Unparsed original string
    };

    /**
     * Parser handles:
     * - Multi-line completeness check (trailing \, unclosed single/double quotes, unclosed $(...) or backticks)
     * - Variable expansion ($VAR, $?, $HOME)
     * - Quoting rules (single quotes preserve literal text, double quotes expand vars and substitutions)
     * - Command substitution ($(...) and backticks `...`) with nesting support
     * - I/O redirection (<, >) and pipelines (|)
     * - Wildcard globbing (*.txt)
     */
    class Parser {
    public:
        using SubshellRunner = std::function<std::string(const std::string&)>;

        /**
         * Check if an entered line has incomplete syntax (needs continuation prompt '>').
         * @param line Accumulated input text.
         * @return True if incomplete, false if ready to execute.
         */
        static bool isInputIncomplete(const std::string& line);

        /**
         * Perform command substitution $(...) and `...` on input string.
         * Recursively evaluates innermost subshells first.
         * @param input Raw text.
         * @param runner Callback function to run subcommands and capture stdout.
         * @return Substituted string.
         */
        static std::string processSubstitutions(const std::string& input, SubshellRunner runner);

        /**
         * Perform parameter expansion ($VAR, $?, $HOME, etc.).
         * Preserves single quotes literally, but expands inside double quotes or unquoted text.
         * @param input String to expand.
         * @param last_exit_status Value of $?.
         * @return Expanded string.
         */
        static std::string expandVariables(const std::string& input, int last_exit_status);

        /**
         * Parse multiWatch syntax:
         * e.g., multiWatch ["cmd1", "cmd2", ...] or multiWatch ["date +%T", "ls | wc -l"]
         * @return Vector of command strings to run in parallel. Empty if not a multiWatch command.
         */
        static std::vector<std::string> parseMultiWatch(const std::string& input);

        /**
         * Parse a full shell command line into a Pipeline structure.
         * Resolves quotes, redirections (<, >), pipes (|), and background flag (&).
         * @param input Full command line.
         * @param last_exit_status Current exit status for $?.
         * @param runner Subshell executor for command substitutions.
         */
        static Pipeline parsePipeline(const std::string& input, int last_exit_status, SubshellRunner runner);

        /**
         * Perform filename globbing on arguments (e.g. *.txt -> file1.txt file2.txt).
         */
        static std::vector<std::string> expandGlobs(const std::vector<std::string>& tokens);

        /**
         * Tokenize a single command segment into arguments, handling quotes and escapes.
         */
        static std::vector<std::string> tokenize(const std::string& segment);
    };

} // namespace Terminator

#endif // TERMINATOR_PARSER_HPP

