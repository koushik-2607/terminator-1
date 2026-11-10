#include "autocomplete.hpp" // Declares the completion API and uses common path/string helpers.
#include <dirent.h>          // Supplies opendir/readdir/closedir for scanning directory entries.
#include <sys/stat.h>        // Supplies stat and S_ISDIR so directories can receive a trailing slash.
#include <algorithm>         // Supplies sorting, deduplication, and min/max algorithms.
#include <unistd.h>          // Supplies access and X_OK for executable checks in PATH.

namespace Terminator {

    const std::vector<std::string> BUILTINS = { // Names recognized internally instead of requiring an external executable.
        "cd", "exit", "history", "multiWatch", "jobs", "fg", "bg", "source", "echo", "pwd", "clear"
    };

    std::string Autocomplete::findLongestCommonPrefix(const std::vector<std::string>& matches) { // Find the shared leading characters across sorted or unsorted candidates.
        if (matches.empty()) return ""; // No candidate means there is no prefix to insert.
        if (matches.size() == 1) return matches[0]; // A single candidate can be completed in full.

        std::string prefix = matches[0]; // Start with the first candidate; each comparison can only shorten it.
        for (size_t i = 1; i < matches.size(); ++i) { // Compare against every other candidate.
            size_t j = 0; // j marks the first character that differs, or the shorter string's end.
            while (j < prefix.length() && j < matches[i].length() && prefix[j] == matches[i][j]) { // Advance while both strings have the same character.
                j++; // Include this matching character in the common prefix.
            }
            prefix = prefix.substr(0, j); // Discard everything after the shared portion.
            if (prefix.empty()) break; // Nothing later can restore a common leading character.
        }
        return prefix; // Return the longest leading sequence common to every candidate.
    }

    void Autocomplete::completeCommands(const std::string& prefix, std::vector<std::string>& matches) { // Collect matching built-ins and executable names from PATH.
        // 1. Built-in shell commands
        for (const auto& b : BUILTINS) { // Inspect every internal command name.
            if (b.rfind(prefix, 0) == 0) { // A match at position zero means b starts with the typed prefix.
                matches.push_back(b); // Add this built-in to the caller's candidate list.
            }
        }

        // 2. Executables in PATH directories
        const char* path_env = getenv("PATH"); // PATH is a colon-separated list of executable search directories.
        if (!path_env) return; // Without PATH there are no external commands to inspect.

        std::vector<std::string> dirs = split(std::string(path_env), ':'); // Convert PATH into one directory per vector element.
        for (const auto& dir : dirs) { // Search each PATH directory independently.
            DIR* dp = opendir(dir.c_str()); // Open the directory stream used by readdir.
            if (!dp) continue; // An unreadable or missing PATH directory contributes no matches.

            struct dirent* entry; // readdir writes a pointer to the current directory-entry record here.
            while ((entry = readdir(dp)) != nullptr) { // A null result means the directory has no more entries.
                std::string name = entry->d_name; // Copy the entry name before the next readdir call reuses its storage.
                if (name == "." || name == "..") continue; // These special entries do not represent useful commands.

                if (name.rfind(prefix, 0) == 0) { // Only candidates beginning with the user's current prefix are relevant.
                    std::string full_path = dir + "/" + name; // Build a path for testing execute permission.
                    // Check if file is executable
                    if (access(full_path.c_str(), X_OK) == 0) { // access returns zero when the current user can execute the path.
                        matches.push_back(name); // Display only the command name, not its PATH directory.
                    }
                }
            }
            closedir(dp); // Release the directory stream and its underlying file descriptor.
        }

        // Deduplicate and sort
        std::sort(matches.begin(), matches.end()); // Sorting is required because std::unique only removes adjacent duplicates.
        matches.erase(std::unique(matches.begin(), matches.end()), matches.end()); // Remove commands found in more than one PATH directory.
    }

    void Autocomplete::completeFiles(const std::string& prefix, std::vector<std::string>& matches, const std::string& cwd) { // Find matching filesystem names in the current tab's working directory or a typed path.
        std::string search_dir = cwd; // Begin in this tab's directory; the GUI passes it because tabs may have different cwd values.
        std::string file_prefix = prefix; // Without a slash, the entire typed word is the filename prefix.

        // Expand ~ if present
        std::string expanded_prefix = expandTilde(prefix); // Use the home directory when searching a word that begins with ~.

        size_t slash_pos = expanded_prefix.rfind('/'); // The last slash divides the directory portion from the filename portion.
        std::string candidate_dir; // Keep the user's original directory spelling for the text inserted into the command line.
        if (slash_pos != std::string::npos) { // A slash means the user is completing a path rather than a bare name.
            candidate_dir = prefix.substr(0, prefix.rfind('/')); // Preserve forms such as ./, ../, or ~/ in the displayed completion.
            search_dir = expanded_prefix.substr(0, slash_pos); // Search the directory named by the typed path.
            if (search_dir.empty()) search_dir = "/"; // A leading slash with no directory prefix means the filesystem root.
            else if (!isAbsolutePath(search_dir)) search_dir = (cwd == "/" ? "/" : cwd + "/") + search_dir; // Resolve relative paths against the tab's cwd.
            file_prefix = expanded_prefix.substr(slash_pos + 1); // Only compare the text after the final slash to entry names.
        }

        DIR* dp = opendir(search_dir.c_str()); // Open the directory selected above for enumeration.
        if (!dp) return; // No results are returned when that path cannot be opened.

        struct dirent* entry; // Directory entry storage is owned by the DIR stream.
        while ((entry = readdir(dp)) != nullptr) { // Process entries until the stream is exhausted.
            std::string name = entry->d_name; // Copy each filename so it remains valid after the next iteration.
            if (name == "." || name == "..") continue; // Do not offer navigation pseudo-entries as completions.

            if (name.rfind(file_prefix, 0) == 0) { // Keep only names whose beginning agrees with what the user typed.
                std::string candidate; // This is the replacement text that will be inserted into the input buffer.
                if (slash_pos != std::string::npos) { // Reattach the original directory prefix when completing a path.
                    candidate = candidate_dir.empty() ? "/" + name : candidate_dir + "/" + name; // Preserve absolute-root form or the typed relative path.
                } else {
                    candidate = name; // A bare filename needs no directory text prepended.
                }

                // Append '/' if it is a directory
                std::string full_target = (search_dir == "/") ? ("/" + name) : (search_dir + "/" + name); // Build the actual path for metadata lookup.
                struct stat st; // stat fills this structure with file type, permissions, size, and timestamps.
                if (stat(full_target.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) { // Check the return code before reading st_mode.
                    candidate += "/"; // A slash signals that the match is a directory and allows continued path completion.
                }

                matches.push_back(candidate); // Add the replacement string to the caller's candidate list.
            }
        }
        closedir(dp); // Close the DIR stream after all candidates have been collected.

        std::sort(matches.begin(), matches.end()); // Make candidate display deterministic and group duplicate paths.
        matches.erase(std::unique(matches.begin(), matches.end()), matches.end()); // Remove duplicate candidate strings.
    }

    Autocomplete::CompletionResult Autocomplete::complete(const std::string& line, size_t cursor_pos, const std::string& cwd) { // Public operation called by the GUI after a Tab keypress.
        CompletionResult result; // Starts with no candidates and is_unique false, as specified by the struct defaults.
        cursor_pos = std::min(cursor_pos, line.size()); // Clamp callers' cursor position to avoid indexing beyond the input string.

        // Find word boundary before cursor
        size_t word_start = cursor_pos; // Walk backward from the cursor to find the start of the word being completed.
        while (word_start > 0 && !std::isspace(line[word_start - 1])) { // Whitespace separates words in this simple completion scanner.
            word_start--; // Include one more character in the word prefix.
        }

        std::string prefix = line.substr(word_start, cursor_pos - word_start); // Extract only the part before the cursor, not the suffix after it.

        completeFiles(prefix, result.candidates, cwd); // This project requirement completes filesystem names, even in command position.

        if (result.candidates.size() == 1) { // A sole match can be inserted without asking the user to choose.
            result.is_unique = true; // Tell the GUI this candidate is unambiguous.
            result.prefix = result.candidates[0]; // Complete the entire filename, including a slash if it is a directory.
        } else if (!result.candidates.empty()) { // Multiple matches can still share a longer prefix than the user typed.
            result.prefix = findLongestCommonPrefix(result.candidates); // Let the GUI expand that shared portion and show choices.
        }

        return result; // Return both the expandable text and the full list needed for selection/display.
    }

} // namespace Terminator


// EOF

// EOF// EOF
