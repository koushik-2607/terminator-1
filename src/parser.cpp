#include "parser.hpp" // Declares shell syntax structures and Parser operations.
#include <sstream> // Provides string streams used to process command text.
#include <cctype> // Provides character tests for whitespace, quoting, and variable names.
#include <stack> // Supports nested syntactic constructs during scanning.
#include <glob.h> // POSIX wildcard expansion API for command arguments.

namespace Terminator {

    /**
     * Determines whether the user input line is incomplete and requires
     * the continuation prompt '>'.
     * Checks for:
     * 1. Trailing unescaped backslash '\'
     * 2. Unclosed single quote (')
     * 3. Unclosed double quote (")
     * 4. Unclosed $(...) parenthesis nesting
     * 5. Unclosed backtick (`)
     */
    bool Parser::isInputIncomplete(const std::string& line) { // GUI calls this after Enter to decide whether to show a continuation prompt.
        if (line.empty()) return false; // Empty input contains no unfinished syntax.

        bool in_single_quote = false; // Single-quoted content is literal until its closing apostrophe.
        bool in_double_quote = false; // Double quotes allow expansions but must still be balanced.
        bool in_backtick = false; // Tracks an unfinished legacy command substitution.
        int subshell_depth = 0; // Counts nested $(...) pairs that remain open.
        bool escaped = false; // Records that a backslash protects the next input byte.

        for (size_t i = 0; i < line.length(); ++i) { // Scan characters sequentially while updating quote/escape state.
            char c = line[i]; // Current byte of the accumulated command line.

            if (escaped) {
                escaped = false; // The current byte consumed the preceding escape.
                continue;
            }

            if (c == '\\' && !in_single_quote) {
                escaped = true; // Treat the following byte literally.
                continue;
            }

            if (c == '\'' && !in_double_quote && !in_backtick) {
                in_single_quote = !in_single_quote; // Toggle only when not inside double quotes or backticks.
            } else if (c == '"' && !in_single_quote) {
                in_double_quote = !in_double_quote; // Toggle only outside single quotes.
            } else if (c == '`' && !in_single_quote) {
                in_backtick = !in_backtick; // Toggle backtick substitution state outside single quotes.
            } else if (c == '$' && !in_single_quote && i + 1 < line.length() && line[i + 1] == '(') {
                subshell_depth++; // Record a nested command-substitution opening.
                i++; // Skip '('
            } else if (c == ')' && !in_single_quote && subshell_depth > 0) {
                subshell_depth--; // A closing parenthesis ends one active substitution level.
            }
        }

        // If line ends with an unescaped backslash, it is incomplete
        if (escaped) return true; // A final backslash needs another line to provide its escaped character.

        // If any quotes or subshells are open, it is incomplete
        return in_single_quote || in_double_quote || in_backtick || (subshell_depth > 0); // Any unmatched opener means the command needs more input.
    }

    /**
     * Executes command substitutions `...` and $(...) innermost first.
     */
    std::string Parser::processSubstitutions(const std::string& input, SubshellRunner runner) {
        std::string result;
        bool in_single_quote = false;
        bool escaped = false;

        for (size_t i = 0; i < input.length(); ++i) {
            char c = input[i];

            if (escaped) {
                result += c;
                escaped = false;
                continue;
            }

            if (c == '\\' && !in_single_quote) {
                escaped = true;
                result += c;
                continue;
            }

            if (c == '\'') {
                in_single_quote = !in_single_quote;
                result += c;
                continue;
            }

            if (in_single_quote) {
                result += c;
                continue;
            }

            // Command substitution via $(...)
            if (c == '$' && i + 1 < input.length() && input[i + 1] == '(') {
                size_t start = i + 2;
                int depth = 1;
                size_t j = start;
                bool sub_single = false, sub_double = false, sub_esc = false;

                while (j < input.length() && depth > 0) {
                    if (sub_esc) { sub_esc = false; j++; continue; }
                    if (input[j] == '\\' && !sub_single) { sub_esc = true; j++; continue; }
                    if (input[j] == '\'' && !sub_double) sub_single = !sub_single;
                    else if (input[j] == '"' && !sub_single) sub_double = !sub_double;
                    else if (!sub_single) {
                        if (input[j] == '$' && j + 1 < input.length() && input[j + 1] == '(') {
                            depth++;
                            j++;
                        } else if (input[j] == ')') {
                            depth--;
                        }
                    }
                    if (depth > 0) j++;
                }

                std::string inner_cmd = input.substr(start, j - start);
                // Recursively process nested substitutions inside inner command
                std::string processed_cmd = processSubstitutions(inner_cmd, runner);
                std::string cmd_output = runner ? runner(processed_cmd) : "";
                
                // Trim trailing newline from command output (bash behavior)
                while (!cmd_output.empty() && (cmd_output.back() == '\n' || cmd_output.back() == '\r')) {
                    cmd_output.pop_back();
                }

                result += cmd_output;
                i = j; // Advance past ')'
                continue;
            }

            // Command substitution via backticks `...`
            if (c == '`') {
                size_t start = i + 1;
                size_t j = start;
                bool backtick_esc = false;

                while (j < input.length()) {
                    if (backtick_esc) { backtick_esc = false; j++; continue; }
                    if (input[j] == '\\') { backtick_esc = true; j++; continue; }
                    if (input[j] == '`') break;
                    j++;
                }

                std::string inner_cmd = input.substr(start, j - start);
                std::string processed_cmd = processSubstitutions(inner_cmd, runner);
                std::string cmd_output = runner ? runner(processed_cmd) : "";

                while (!cmd_output.empty() && (cmd_output.back() == '\n' || cmd_output.back() == '\r')) {
                    cmd_output.pop_back();
                }

                result += cmd_output;
                i = j; // Advance past '`'
                continue;
            }

            result += c;
        }

        return result;
    }

    /**
     * Expands environment variables and $? according to bash quoting rules.
     * Single quotes: literal, NO expansion.
     * Double quotes: expands $VAR.
     * Unquoted: expands $VAR.
     */
    std::string Parser::expandVariables(const std::string& input, int last_exit_status, const VariableMap* variables) { // Expand shell parameters while retaining quote syntax for tokenization.
        std::string result; // Receives original characters and expanded values.
        bool in_single_quote = false; // Prevent parameter expansion in single-quoted text.
        bool in_double_quote = false; // Permit expansion inside double quotes.
        bool escaped = false; // Protect an escaped dollar sign from expansion.

        for (size_t i = 0; i < input.length(); ++i) { // Walk the command and maintain quote context.
            char c = input[i]; // Current input byte.

            if (escaped) {
                result += c; // Preserve escaped content in the resulting command string.
                escaped = false; // The current character has consumed the escape.
                continue;
            }

            if (c == '\\' && !in_single_quote) {
                escaped = true; // The next byte must not be interpreted as an expansion marker.
                result += c; // Retain the backslash for the tokenizer's later escape handling.
                continue;
            }

            if (c == '\'' && !in_double_quote) {
                in_single_quote = !in_single_quote; // Toggle single-quoted literal mode.
                result += c; // Keep delimiters until tokenization removes them.
                continue;
            }

            if (c == '"' && !in_single_quote) {
                in_double_quote = !in_double_quote; // Toggle double-quoted mode, unless currently in a single-quoted region.
                result += c; // Preserve the quote syntax for later word splitting.
                continue;
            }

            // Perform expansion if outside single quotes
            if (c == '$' && !in_single_quote && i + 1 < input.length()) {
                if (std::isdigit(static_cast<unsigned char>(input[i + 1]))) { // Positional parameter names may contain one or more digits.
                    size_t j = i + 1;
                    while (j < input.size() && std::isdigit(static_cast<unsigned char>(input[j]))) ++j;
                    std::string key = input.substr(i + 1, j - i - 1);
                    auto local = variables ? variables->find(key) : VariableMap::const_iterator{};
                    if (variables && local != variables->end()) result += local->second; // Prefer this tab's variable over process-global state.
                    else if (const char* value = getenv(key.c_str())) result += value; // Fall back to inherited environment variables.
                    i = j - 1;
                    continue;
                }
                if (input[i + 1] == '#') { // $# reports the active script's number of positional arguments.
                    auto local = variables ? variables->find("#") : VariableMap::const_iterator{};
                    if (variables && local != variables->end()) result += local->second;
                    else if (const char* value = getenv("#")) result += value;
                    ++i;
                    continue;
                }
                // Check for $?
                if (input[i + 1] == '?') {
                    result += std::to_string(last_exit_status); // Replace $? with the last command's numeric exit code.
                    i++;
                    continue;
                }

                // Check for ${VAR}
                if (input[i + 1] == '{') {
                    size_t close_brace = input.find('}', i + 2);
                    if (close_brace != std::string::npos) {
                        std::string var_name = input.substr(i + 2, close_brace - (i + 2));
                        auto local = variables ? variables->find(var_name) : VariableMap::const_iterator{};
                        if (variables && local != variables->end()) result += local->second;
                        else if (const char* val = getenv(var_name.c_str())) result += val;
                        i = close_brace;
                        continue;
                    }
                }

                // Check for standard identifier characters: [a-zA-Z_][a-zA-Z0-9_]*
                if (std::isalpha(input[i + 1]) || input[i + 1] == '_') {
                    size_t j = i + 1;
                    while (j < input.length() && (std::isalnum(input[j]) || input[j] == '_')) {
                        j++;
                    }
                    std::string var_name = input.substr(i + 1, j - (i + 1));
                    auto local = variables ? variables->find(var_name) : VariableMap::const_iterator{};
                    if (variables && local != variables->end()) result += local->second;
                    else if (const char* val = getenv(var_name.c_str())) result += val;
                    i = j - 1;
                    continue;
                }
            }

            result += c;
        }

        return result;
    }

    /**
     * Parses multiWatch syntax:
     * multiWatch ["cmd1", "cmd2", "ls | wc -l"]
     */
    std::vector<std::string> Parser::parseMultiWatch(const std::string& input) {
        std::vector<std::string> commands;
        std::string trimmed = trim(input);

        if (trimmed.rfind("multiWatch", 0) != 0 && trimmed.rfind("multiwatch", 0) != 0) {
            return commands;
        }

        size_t open_bracket = trimmed.find('[');
        size_t close_bracket = trimmed.rfind(']');

        if (open_bracket == std::string::npos || close_bracket == std::string::npos || close_bracket <= open_bracket) {
            return commands;
        }

        std::string list_content = trimmed.substr(open_bracket + 1, close_bracket - open_bracket - 1);
        bool in_quote = false;
        char quote_char = 0;
        std::string current_cmd;
        bool escaped = false;

        for (size_t i = 0; i < list_content.length(); ++i) {
            char c = list_content[i];

            if (escaped) {
                current_cmd += c;
                escaped = false;
                continue;
            }

            if (c == '\\') {
                escaped = true;
                continue;
            }

            if (!in_quote && (c == '"' || c == '\'')) {
                in_quote = true;
                quote_char = c;
            } else if (in_quote && c == quote_char) {
                in_quote = false;
                commands.push_back(current_cmd);
                current_cmd.clear();
            } else if (in_quote) {
                current_cmd += c;
            }
        }

        return commands;
    }

    /**
     * Tokenizes command into arguments, removing quotes while respecting escaping.
     */
    std::vector<std::string> Parser::tokenize(const std::string& segment) {
        std::vector<std::string> tokens;
        std::string current;
        bool in_single = false;
        bool in_double = false;
        bool escaped = false;
        bool token_has_content = false;

        for (size_t i = 0; i < segment.length(); ++i) {
            char c = segment[i];

            if (escaped) {
                current += c;
                token_has_content = true;
                escaped = false;
                continue;
            }

            if (c == '\\' && !in_single) {
                escaped = true;
                continue;
            }

            if (c == '\'' && !in_double) {
                in_single = !in_single;
                token_has_content = true; // Even '' produces an argument
                continue;
            }

            if (c == '"' && !in_single) {
                in_double = !in_double;
                token_has_content = true; // Even "" produces an argument
                continue;
            }

            if (std::isspace(c) && !in_single && !in_double) {
                if (token_has_content) {
                    tokens.push_back(current);
                    current.clear();
                    token_has_content = false;
                }
            } else {
                current += c;
                token_has_content = true;
            }
        }

        if (token_has_content) {
            tokens.push_back(current);
        }

        return tokens;
    }

    /**
     * Expands wildcard file glob patterns (e.g. *.txt).
     */
    std::vector<std::string> Parser::expandGlobs(const std::vector<std::string>& tokens) {
        std::vector<std::string> expanded;

        for (const auto& token : tokens) {
            if (token.find_first_of("*?[]") != std::string::npos) {
                glob_t globbuf;
                int ret = glob(token.c_str(), GLOB_NOCHECK | GLOB_TILDE, nullptr, &globbuf);
                if (ret == 0) {
                    for (size_t j = 0; j < globbuf.gl_pathc; ++j) {
                        expanded.emplace_back(globbuf.gl_pathv[j]);
                    }
                    globfree(&globbuf);
                    continue;
                }
            }
            expanded.push_back(token);
        }

        return expanded;
    }

    /**
     * Parses a command line into a Pipeline.
     */
    Pipeline Parser::parsePipeline(const std::string& input, int last_exit_status, SubshellRunner runner,
                                   const VariableMap* variables) {
        Pipeline pipeline;
        pipeline.raw_command = input;

        // Step 1: Clean trailing continuation slashes if present
        std::string cleaned;
        bool in_d = false;
        bool in_s = false;
        int sub_paren_depth = 0;

        for (size_t i = 0; i < input.length(); ++i) {
            char c = input[i];
            if (c == '\'' && !in_d) in_s = !in_s;
            else if (c == '"' && !in_s) in_d = !in_d;
            else if (c == '$' && i + 1 < input.length() && input[i + 1] == '(' && !in_s) {
                sub_paren_depth++;
                cleaned += c;
                cleaned += input[++i];
                continue;
            } else if (c == ')' && sub_paren_depth > 0 && !in_s) {
                sub_paren_depth--;
            }

            if (c == '\\' && i + 1 < input.length() && input[i + 1] == '\n') {
                if (in_d && sub_paren_depth == 0) {
                    // Inside quotes (not in subshell), preserve newline continuation
                    cleaned += '\n';
                }
                i++; // Skip newline
                continue;
            }
            cleaned += c;
        }

        // Step 2: Process substitutions $(...) and `...`
        std::string substituted = processSubstitutions(cleaned, runner);

        // Step 3: Expand $VAR and $?
        std::string expanded = expandVariables(substituted, last_exit_status, variables);

        // Step 4: Check for background execution '&' at the end
        std::string trimmed = trim(expanded);
        if (!trimmed.empty() && trimmed.back() == '&') {
            pipeline.run_in_background = true;
            trimmed.pop_back();
            trimmed = trim(trimmed);
        }

        // Step 5: Split by pipe '|', taking care not to split inside quotes
        std::vector<std::string> pipe_segments;
        std::string seg;
        bool in_single = false, in_double = false, escaped = false;

        for (size_t i = 0; i < trimmed.length(); ++i) {
            char c = trimmed[i];
            if (escaped) { seg += c; escaped = false; continue; }
            if (c == '\\' && !in_single) { escaped = true; seg += c; continue; }
            if (c == '\'' && !in_double) { in_single = !in_single; seg += c; continue; }
            if (c == '"' && !in_single) { in_double = !in_double; seg += c; continue; }

            if (c == '|' && !in_single && !in_double) {
                pipe_segments.push_back(trim(seg));
                seg.clear();
            } else {
                seg += c;
            }
        }
        if (!seg.empty() || pipe_segments.empty()) {
            pipe_segments.push_back(trim(seg));
        }

        // Step 6: Parse each command segment for redirections (<, >) and arguments
        for (const auto& cmd_text : pipe_segments) {
            Command cmd;
            std::string clean_cmd_text;
            in_single = false;
            in_double = false;
            escaped = false;

            for (size_t i = 0; i < cmd_text.length(); ++i) {
                char c = cmd_text[i];
                if (escaped) { clean_cmd_text += c; escaped = false; continue; }
                if (c == '\\' && !in_single) { escaped = true; clean_cmd_text += c; continue; }
                if (c == '\'' && !in_double) { in_single = !in_single; clean_cmd_text += c; continue; }
                if (c == '"' && !in_single) { in_double = !in_double; clean_cmd_text += c; continue; }

                if (c == '<' && !in_single && !in_double) {
                    // Input redirection
                    size_t j = i + 1;
                    while (j < cmd_text.length() && std::isspace(cmd_text[j])) j++;
                    std::string file_target;
                    while (j < cmd_text.length() && !std::isspace(cmd_text[j]) && cmd_text[j] != '>' && cmd_text[j] != '<') {
                        file_target += cmd_text[j++];
                    }
                    std::string target = trim(file_target);
                    if (target.length() >= 2 && 
                        ((target.front() == '"' && target.back() == '"') || 
                         (target.front() == '\'' && target.back() == '\''))) {
                        target = target.substr(1, target.length() - 2);
                    }
                    cmd.input_file = target;
                    i = j - 1;
                } else if (c == '>' && !in_single && !in_double) {
                    // Output redirection
                    bool is_append = false;
                    if (i + 1 < cmd_text.length() && cmd_text[i + 1] == '>') {
                        is_append = true;
                        i++;
                    }
                    cmd.append_output = is_append;
                    size_t j = i + 1;
                    while (j < cmd_text.length() && std::isspace(cmd_text[j])) j++;
                    std::string file_target;
                    while (j < cmd_text.length() && !std::isspace(cmd_text[j]) && cmd_text[j] != '>' && cmd_text[j] != '<') {
                        file_target += cmd_text[j++];
                    }
                    std::string target = trim(file_target);
                    if (target.length() >= 2 && 
                        ((target.front() == '"' && target.back() == '"') || 
                         (target.front() == '\'' && target.back() == '\''))) {
                        target = target.substr(1, target.length() - 2);
                    }
                    cmd.output_file = target;
                    i = j - 1;
                } else {
                    clean_cmd_text += c;
                }
            }

            // Tokenize arguments and expand globs
            std::vector<std::string> raw_tokens = tokenize(clean_cmd_text);
            cmd.args = expandGlobs(raw_tokens);

            if (!cmd.args.empty() || !cmd.input_file.empty() || !cmd.output_file.empty()) {
                pipeline.commands.push_back(cmd);
            }
        }

        return pipeline;
    }

} // namespace Terminator
// EOF
