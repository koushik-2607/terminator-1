#include "autocomplete.hpp"
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <unistd.h>

namespace Terminator {

    const std::vector<std::string> BUILTINS = {
        "cd", "exit", "history", "multiWatch", "jobs", "fg", "bg", "source", "echo", "pwd", "clear"
    };

    std::string Autocomplete::findLongestCommonPrefix(const std::vector<std::string>& matches) {
        if (matches.empty()) return "";
        if (matches.size() == 1) return matches[0];

        std::string prefix = matches[0];
        for (size_t i = 1; i < matches.size(); ++i) {
            size_t j = 0;
            while (j < prefix.length() && j < matches[i].length() && prefix[j] == matches[i][j]) {
                j++;
            }
            prefix = prefix.substr(0, j);
            if (prefix.empty()) break;
        }
        return prefix;
    }

    void Autocomplete::completeCommands(const std::string& prefix, std::vector<std::string>& matches) {
        // 1. Built-in shell commands
        for (const auto& b : BUILTINS) {
            if (b.rfind(prefix, 0) == 0) {
                matches.push_back(b);
            }
        }

        // 2. Executables in PATH directories
        const char* path_env = getenv("PATH");
        if (!path_env) return;

        std::vector<std::string> dirs = split(std::string(path_env), ':');
        for (const auto& dir : dirs) {
            DIR* dp = opendir(dir.c_str());
            if (!dp) continue;

            struct dirent* entry;
            while ((entry = readdir(dp)) != nullptr) {
                std::string name = entry->d_name;
                if (name == "." || name == "..") continue;

                if (name.rfind(prefix, 0) == 0) {
                    std::string full_path = dir + "/" + name;
                    // Check if file is executable
                    if (access(full_path.c_str(), X_OK) == 0) {
                        matches.push_back(name);
                    }
                }
            }
            closedir(dp);
        }

        // Deduplicate and sort
        std::sort(matches.begin(), matches.end());
        matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
    }

    void Autocomplete::completeFiles(const std::string& prefix, std::vector<std::string>& matches) {
        std::string search_dir = ".";
        std::string file_prefix = prefix;

        // Expand ~ if present
        std::string expanded_prefix = expandTilde(prefix);

        size_t slash_pos = expanded_prefix.rfind('/');
        if (slash_pos != std::string::npos) {
            search_dir = expanded_prefix.substr(0, slash_pos);
            if (search_dir.empty()) search_dir = "/";
            file_prefix = expanded_prefix.substr(slash_pos + 1);
        }

        DIR* dp = opendir(search_dir.c_str());
        if (!dp) return;

        struct dirent* entry;
        while ((entry = readdir(dp)) != nullptr) {
            std::string name = entry->d_name;
            if (name == "." || name == "..") continue;

            if (name.rfind(file_prefix, 0) == 0) {
                std::string candidate;
                if (slash_pos != std::string::npos) {
                    if (search_dir == "/") {
                        candidate = "/" + name;
                    } else {
                        candidate = search_dir + "/" + name;
                    }
                } else {
                    candidate = name;
                }

                // Append '/' if it is a directory
                std::string full_target = (search_dir == "/") ? ("/" + name) : (search_dir + "/" + name);
                struct stat st;
                if (stat(full_target.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                    candidate += "/";
                }

                matches.push_back(candidate);
            }
        }
        closedir(dp);

        std::sort(matches.begin(), matches.end());
        matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
    }

    Autocomplete::CompletionResult Autocomplete::complete(const std::string& line, size_t cursor_pos) {
        CompletionResult result;

        // Find word boundary before cursor
        size_t word_start = cursor_pos;
        while (word_start > 0 && !std::isspace(line[word_start - 1])) {
            word_start--;
        }

        std::string prefix = line.substr(word_start, cursor_pos - word_start);

        // Check if completing command (first token on line) or file/argument
        std::string text_before = trim(line.substr(0, word_start));
        bool is_command_position = text_before.empty() || text_before.back() == '|' || text_before.back() == ';';

        if (is_command_position && prefix.find('/') == std::string::npos) {
            completeCommands(prefix, result.candidates);
        } else {
            completeFiles(prefix, result.candidates);
        }

        if (result.candidates.size() == 1) {
            result.is_unique = true;
            result.prefix = result.candidates[0];
        } else if (!result.candidates.empty()) {
            result.prefix = findLongestCommonPrefix(result.candidates);
        }

        return result;
    }

} // namespace Terminator

