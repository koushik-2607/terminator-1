#ifndef TERMINATOR_HISTORY_HPP // Avoid duplicate layout/class declarations across modules.
#define TERMINATOR_HISTORY_HPP // Mark this header as included in the current compilation.

#include "common.hpp" // Defines shared history limits and includes POSIX semaphore/memory declarations.
#include <string> // Represents command lines and shared-memory names safely in C++.
#include <vector> // Returns history snapshots without exposing internal shared-memory storage.

namespace Terminator {

    /**
     * Shared memory block layout for command history.
     * Stored in POSIX shared memory, allowing both
     * multi-threaded and multi-process modes to access command
     * history atomically and consistently.
     */
    struct SharedHistoryLayout { // Exact data layout mapped into POSIX shared memory by the GUI and tabs.
        sem_t lock; // Process-shared semaphore serializes access across threads and forked processes.
        uint32_t count; // Number of valid entries, capped at MAX_HISTORY_ENTRIES.
        char entries[MAX_HISTORY_ENTRIES][MAX_HISTORY_LINE_LEN]; // Fixed-size command records avoid pointers that cannot be shared between processes.
    };

    /**
     * HistoryManager manages the persistent and shared command history.
     * In Thread Mode: History is shared across threads.
     * In Process Mode: History is shared across tab processes via POSIX shared memory.
     * All read and write operations are guarded by a process-shared semaphore.
     */
    class HistoryManager { // Owns or attaches to shared history memory and its persistent file representation.
    public:
        /**
         * Initialize the history manager.
         * @param is_parent True if GUI parent process creates/initializes the shared memory.
         * @param shm_identifier Unique identifier suffix for POSIX shared memory name.
         */
        HistoryManager(bool is_parent = true, const std::string& shm_identifier = ""); // Parent creates/initializes; child mode attaches using the same name.

        /**
         * Destructor: unmaps shared memory and unlinks POSIX shm object if parent.
         */
        ~HistoryManager(); // Unmap memory, close the descriptor, and let the creator unlink shared memory.

        /**
         * Add a new command line to history (atomically).
         * Ignores empty commands or identical consecutive commands.
         * @param command The command string typed by user.
         */
        void add(const std::string& command); // Lock, append/evict, and persist one nonempty command.

        /**
         * Retrieve all history entries in chronological order.
         */
        std::vector<std::string> getAll(); // Return a locked chronological copy so callers do not hold the semaphore while rendering.

        // Exact newest match, otherwise all commands tied for best substring (length > 2).
        std::vector<std::string> search(const std::string& term); // Prefer newest exact match, else return all longest-common-substring ties >2 chars.

        /**
         * Get history entry by 0-based index.
         */
        std::string get(size_t index); // Safely read one zero-based entry, returning empty for an out-of-range index.

        /**
         * Return total number of commands stored in history.
         */
        size_t size(); // Read the current entry count while holding the process-shared lock.

        /**
         * Get the shared memory object name used by this instance.
         */
        std::string getShmName() const { return shm_name_; } // Expose the identifier for creating compatible child attachments.

    private:
        std::string shm_name_; // POSIX shared-memory object name, beginning with '/' as required by shm_open.
        int shm_fd_; // File descriptor returned by shm_open; -1 indicates no descriptor is open.
        SharedHistoryLayout* shared_data_; // mmap address of the shared structure, or null/failed when initialization fails.
        bool is_creator_; // True only for the process responsible for initialization and final unlink.
    };

} // namespace Terminator

#endif // TERMINATOR_HISTORY_HPP closes the header guard.

// EOF
