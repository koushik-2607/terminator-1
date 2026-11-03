#ifndef TERMINATOR_COMMON_HPP
#define TERMINATOR_COMMON_HPP

#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <memory>
#include <cstring>
#include <cstdint>
#include <climits>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <signal.h>
#include <poll.h>
#include <glob.h>
#include <termios.h>

/**
 * Terminator Terminal Constants and Configuration Limits
 */
namespace Terminator {

    // Maximum history capacity stored in shared memory
    constexpr size_t MAX_HISTORY_ENTRIES = 1000;
    constexpr size_t MAX_HISTORY_LINE_LEN = 1024;

    // Buffer limits for IPC pipes and I/O reading
    constexpr size_t BUFFER_SIZE = 4096;

    // Default multiWatch re-run interval (in seconds)
    constexpr int MULTIWATCH_INTERVAL_SEC = 2;

    // Terminal default dimensions in pixels
    constexpr int DEFAULT_WINDOW_WIDTH  = 900;
    constexpr int DEFAULT_WINDOW_HEIGHT = 600;

    // Default dimensions in character cells
    constexpr int DEFAULT_COLS = 80;
    constexpr int DEFAULT_ROWS = 24;

    // Tab bar height in pixels
    constexpr int TAB_BAR_HEIGHT = 28;

    /**
     * Escape sequence marker used to send internal control messages
     * from Tab Shell process/thread back to GUI (e.g. title changes, prompt changes).
     */
    const std::string OSC_TITLE_PREFIX = "\033]0;";
    const std::string OSC_TITLE_SUFFIX = "\007";

    /**
     * Helper function: Check if a path is absolute.
     */
    inline bool isAbsolutePath(const std::string& path) {
        return !path.empty() && path[0] == '/';
    }

    /**
     * Helper function: Expand tilde (~) at beginning of path to user's home directory.
     */
    inline std::string expandTilde(const std::string& path) {
        if (path.empty()) return path;
        if (path[0] == '~') {
            const char* home = getenv("HOME");
            if (!home) home = "/";
            if (path.length() == 1) return std::string(home);
            if (path[1] == '/') return std::string(home) + path.substr(1);
        }
        return path;
    }

    /**
     * Helper function: Shorten path for tab title display:
     * e.g., /home/user/Downloads -> ~/Downloads
     */
    inline std::string shortenPath(const std::string& path) {
        const char* home = getenv("HOME");
        if (home && std::string(home).length() > 0) {
            std::string homeStr(home);
            if (path == homeStr) {
                return "~";
            }
            if (path.rfind(homeStr, 0) == 0 && path.length() > homeStr.length() && path[homeStr.length()] == '/') {
                return "~" + path.substr(homeStr.length());
            }
        }
        return path;
    }

    /**
     * Helper function: Trim leading and trailing whitespace from a string.
     */
    inline std::string trim(const std::string& str) {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return "";
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, (last - first + 1));
    }

    /**
     * Helper function: Split string by delimiter.
     */
    inline std::vector<std::string> split(const std::string& s, char delimiter) {
        std::vector<std::string> tokens;
        std::string token;
        std::istringstream tokenStream(s);
        while (std::getline(tokenStream, token, delimiter)) {
            tokens.push_back(token);
        }
        return tokens;
    }
}

#endif // TERMINATOR_COMMON_HPP
