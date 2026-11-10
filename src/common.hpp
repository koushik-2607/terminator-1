#ifndef TERMINATOR_COMMON_HPP // Header guard avoids duplicate definitions when many modules include common utilities.
#define TERMINATOR_COMMON_HPP // Marks this header as already included in the current translation unit.

#include <string>      // C++ text values are used for paths, commands, and terminal output.
#include <vector>      // Dynamic arrays hold arguments, lines, and tab/history data.
#include <iostream>    // Shared modules use the standard streams for diagnostics and output.
#include <sstream>     // Stream-based formatting and token parsing helpers use string streams.
#include <memory>      // Smart pointers manage dynamically allocated GUI and tab objects.
#include <cstring>     // C string copying and memory operations support fixed-size shared-memory records.
#include <cstdint>     // Fixed-width integer types make shared-memory fields predictable.
#include <climits>     // Defines system limits such as PATH_MAX.
#include <unistd.h>    // POSIX process, descriptor, and path functions such as fork, read, write, and getcwd.
#include <fcntl.h>     // File descriptor flags and open() options for redirection and pipes.
#include <sys/types.h> // POSIX types such as pid_t and size_t used in system-call interfaces.
#include <sys/wait.h>  // waitpid and wait-status macros used for child/job management.
#include <sys/stat.h>  // File metadata calls and macros used to test file types.
#include <sys/mman.h>  // mmap/shared-memory mapping flags for cross-process history.
#include <semaphore.h> // POSIX semaphore declarations used to synchronize shared history.
#include <signal.h>   // Signal constants and handlers used for Ctrl+C/Ctrl+Z process control.
#include <poll.h>     // pollfd and poll() support event-driven descriptor waits without busy loops.
#include <glob.h>     // POSIX wildcard expansion declarations used by the parser.
#include <termios.h>  // Terminal-control definitions available to shell and terminal components.

/**
 * Terminator Terminal Constants and Configuration Limits
 */
namespace Terminator {

    // Maximum history capacity stored in shared memory
    constexpr size_t MAX_HISTORY_ENTRIES = 10000; // Cap shared/persistent history so its memory use remains bounded.
    constexpr size_t MAX_HISTORY_LINE_LEN = 1024; // Bound each fixed-width history record stored in shared memory.

    // Buffer limits for IPC pipes and I/O reading
    constexpr size_t BUFFER_SIZE = 4096; // Standard chunk size for pipe reads and IPC text buffers.

    // Default multiWatch re-run interval (in seconds)
    constexpr int MULTIWATCH_INTERVAL_SEC = 2; // Delay between relaunching completed multiWatch commands.

    // Terminal default dimensions in pixels
    constexpr int DEFAULT_WINDOW_WIDTH  = 900; // Initial X11 window width in pixels.
    constexpr int DEFAULT_WINDOW_HEIGHT = 600; // Initial X11 window height in pixels.

    // Default dimensions in character cells
    constexpr int DEFAULT_COLS = 80; // Conventional terminal width in character cells.
    constexpr int DEFAULT_ROWS = 24; // Conventional terminal height in character cells.

    // Tab bar height in pixels
    constexpr int TAB_BAR_HEIGHT = 28; // Pixel height reserved at the top of the X11 window for tabs.

    /**
     * Escape sequence marker used to send internal control messages
     * from Tab Shell process/thread back to GUI (e.g. title changes, prompt changes).
     */
    const std::string OSC_TITLE_PREFIX = "\033]0;"; // Begins an OSC terminal-control sequence used internally to update a tab title.
    const std::string OSC_TITLE_SUFFIX = "\007"; // BEL terminates the OSC sequence before the GUI strips and handles it.

    /**
     * Helper function: Check if a path is absolute.
     */
    inline bool isAbsolutePath(const std::string& path) { // Unix absolute paths begin at the filesystem root.
        return !path.empty() && path[0] == '/'; // Empty strings are not absolute; otherwise inspect the first character.
    }

    /**
     * Helper function: Expand tilde (~) at beginning of path to user's home directory.
     */
    inline std::string expandTilde(const std::string& path) { // Expand the common ~ and ~/path forms using the current user's HOME variable.
        if (path.empty()) return path; // There is nothing to expand for an empty path.
        if (path[0] == '~') { // Only a leading tilde is treated specially; other tildes remain literal.
            const char* home = getenv("HOME"); // getenv returns a process-environment pointer, or null if HOME is unset.
            if (!home) home = "/"; // Use a safe absolute fallback instead of dereferencing null.
            if (path.length() == 1) return std::string(home); // A lone tilde means exactly the home directory.
            if (path[1] == '/') return std::string(home) + path.substr(1); // Preserve the remainder after replacing ~.
        }
        return path;
    }

    /**
     * Helper function: Shorten path for tab title display:
     * e.g., /home/user/Downloads -> ~/Downloads
     */
    inline std::string shortenPath(const std::string& path) { // Convert paths inside HOME into a compact form for the tab label.
        const char* home = getenv("HOME"); // Read HOME from the environment; it may be unset.
        if (home && std::string(home).length() > 0) { // Only compare when HOME is a nonempty string.
            std::string homeStr(home); // Own a C++ copy for safe length and prefix operations.
            if (path == homeStr) { // The home directory itself has a concise spelling.
                return "~"; // Display home as a single tilde.
            }
            if (path.rfind(homeStr, 0) == 0 && path.length() > homeStr.length() && path[homeStr.length()] == '/') { // Require a path-component boundary so /home/u2 is not shortened as /home/u.
                return "~" + path.substr(homeStr.length()); // Replace only the HOME prefix and retain the child path.
            }
        }
        return path;
    }

    /**
     * Helper function: Trim leading and trailing whitespace from a string.
     */
    inline std::string trim(const std::string& str) { // Return a copy with leading/trailing spaces and line whitespace removed.
        size_t first = str.find_first_not_of(" \t\r\n"); // Locate the first character that is not space, tab, CR, or LF.
        if (first == std::string::npos) return ""; // All-whitespace input has no retained range.
        size_t last = str.find_last_not_of(" \t\r\n"); // Locate the final non-whitespace character.
        return str.substr(first, (last - first + 1)); // Extract the inclusive meaningful range using string's length-based API.
    }

    /**
     * Helper function: Split string by delimiter.
     */
    inline std::vector<std::string> split(const std::string& s, char delimiter) { // Break a string into pieces around one separator character.
        std::vector<std::string> tokens; // Store each extracted piece in order.
        std::string token; // getline writes the next piece here on every iteration.
        std::istringstream tokenStream(s); // Treat the input string as a readable stream.
        while (std::getline(tokenStream, token, delimiter)) { // Stop when no more delimiter-separated pieces can be read.
            tokens.push_back(token); // Preserve the current piece, including an empty piece between adjacent delimiters.
        }
        return tokens;
    }
}

#endif // TERMINATOR_COMMON_HPP
// EOF
