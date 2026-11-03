#ifndef TERMINATOR_HISTORY_HPP
#define TERMINATOR_HISTORY_HPP

#include "common.hpp"
#include <string>
#include <vector>

namespace Terminator {

    /**
     * Shared memory block layout for command history.
     * Stored in POSIX shared memory, allowing both
     * multi-threaded and multi-process modes to access command
     * history atomically and consistently.
     */
    struct SharedHistoryLayout {
        sem_t lock;                                                  // POSIX process-shared semaphore (pshared = 1)
        uint32_t count;                                              // Total number of history entries
        char entries[MAX_HISTORY_ENTRIES][MAX_HISTORY_LINE_LEN];    // Ring/linear array of stored command lines
    };

    /**
     * HistoryManager manages the persistent and shared command history.
     * In Thread Mode: History is shared across threads.
     * In Process Mode: History is shared across tab processes via POSIX shared memory.
     * All read and write operations are guarded by a process-shared semaphore.
     */
    class HistoryManager {
    public:
        /**
         * Initialize the history manager.
         * @param is_parent True if GUI parent process creates/initializes the shared memory.
         * @param shm_identifier Unique identifier suffix for POSIX shared memory name.
         */
        HistoryManager(bool is_parent = true, const std::string& shm_identifier = "");

        /**
         * Destructor: unmaps shared memory and unlinks POSIX shm object if parent.
         */
        ~HistoryManager();

        /**
         * Add a new command line to history (atomically).
         * Ignores empty commands or identical consecutive commands.
         * @param command The command string typed by user.
         */
        void add(const std::string& command);

        /**
         * Retrieve all history entries in chronological order.
         */
        std::vector<std::string> getAll();

        /**
         * Get history entry by 0-based index.
         */
        std::string get(size_t index);

        /**
         * Return total number of commands stored in history.
         */
        size_t size();

        /**
         * Get the shared memory object name used by this instance.
         */
        std::string getShmName() const { return shm_name_; }

    private:
        std::string shm_name_;
        int shm_fd_;
        SharedHistoryLayout* shared_data_;
        bool is_creator_;
    };

} // namespace Terminator

#endif // TERMINATOR_HISTORY_HPP

