#ifndef TERMINATOR_PARSER_HPP // Prevent duplicate command and parser declarations in translation units.
#define TERMINATOR_PARSER_HPP // Set this header's unique include-guard marker.

#include "common.hpp" // Supplies shared string/path helpers and POSIX types used by parsing.
#include <string> // Command text, tokens, arguments, and redirection paths use std::string.
#include <vector> // Commands and arguments are stored in dynamically sized ordered collections.
#include <functional> // SubshellRunner is a callback object supplied by ShellEngine.
#include <unordered_map> // VariableMap stores shell-local script variables by name.

namespace Terminator {

    /**
     * Single command within a pipeline, including its arguments and redirections.
     */
    struct Command { // Parsed stage in a command line, before ShellEngine creates child processes.
        std::vector<std::string> args; // argv-like command name at index zero followed by arguments.
        std::string input_file; // Source path to connect to this command's standard input for '<'.
        std::string output_file; // Destination path to connect to standard output for '>' or '>>'.
        bool append_output = false; // True selects O_APPEND instead of truncating the output file.
    };

    /**
     * Pipeline represents a series of commands connected by '|'.
     */
    struct Pipeline { // Represents a whole parsed line and its connected command stages.
        std::vector<Command> commands; // Stages run left-to-right and are connected with pipes by ShellEngine.
        bool run_in_background = false; // True when a trailing ampersand asks the shell not to wait in foreground.
        std::string raw_command; // Original input retained for job labels and diagnostics.
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
    class Parser { // Static parsing helpers shared by the GUI/shell and unit tests.
    public:
        using SubshellRunner = std::function<std::string(const std::string&)>; // Callback executes substitution text and returns captured stdout.
        using VariableMap = std::unordered_map<std::string, std::string>; // Per-shell variables override process environment variables.

        /**
         * Check if an entered line has incomplete syntax (needs continuation prompt '>').
         * @param line Accumulated input text.
         * @return True if incomplete, false if ready to execute.
         */
        static bool isInputIncomplete(const std::string& line); // Detect unfinished quoting, substitutions, or trailing line continuation.

        /**
         * Perform command substitution $(...) and `...` on input string.
         * Recursively evaluates innermost subshells first.
         * @param input Raw text.
         * @param runner Callback function to run subcommands and capture stdout.
         * @return Substituted string.
         */
        static std::string processSubstitutions(const std::string& input, SubshellRunner runner); // Evaluate $(...) and backticks from inner expressions outward.

        /**
         * Perform parameter expansion ($VAR, $?, $HOME, etc.).
         * Preserves single quotes literally, but expands inside double quotes or unquoted text.
         * @param input String to expand.
         * @param last_exit_status Value of $?.
         * @return Expanded string.
         */
        static std::string expandVariables(const std::string& input, int last_exit_status, // Expand $?, environment names, and optional local variables.
                                           const VariableMap* variables = nullptr); // Null means consult only the process environment.

        /**
         * Parse multiWatch syntax:
         * e.g., multiWatch ["cmd1", "cmd2", ...] or multiWatch ["date +%T", "ls | wc -l"]
         * @return Vector of command strings to run in parallel. Empty if not a multiWatch command.
         */
        static std::vector<std::string> parseMultiWatch(const std::string& input); // Extract quoted commands from the project's multiWatch syntax.

        /**
         * Parse a full shell command line into a Pipeline structure.
         * Resolves quotes, redirections (<, >), pipes (|), and background flag (&).
         * @param input Full command line.
         * @param last_exit_status Current exit status for $?.
         * @param runner Subshell executor for command substitutions.
         */
        static Pipeline parsePipeline(const std::string& input, int last_exit_status, SubshellRunner runner, // Parse a complete line into commands, pipes, and redirections.
                                      const VariableMap* variables = nullptr); // Optional local-variable overlay for the tab executing this line.

        /**
         * Perform filename globbing on arguments (e.g. *.txt -> file1.txt file2.txt).
         */
        static std::vector<std::string> expandGlobs(const std::vector<std::string>& tokens); // Replace wildcard tokens such as *.txt with matching pathnames.

        /**
         * Tokenize a single command segment into arguments, handling quotes and escapes.
         */
        static std::vector<std::string> tokenize(const std::string& segment); // Split one command stage while removing syntax quotes and honoring escapes.
    };

} // namespace Terminator

#endif // TERMINATOR_PARSER_HPP closes the parser header guard.

// EOF
