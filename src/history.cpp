#include "history.hpp" // Declares the shared-memory layout and HistoryManager interface.
#include <iostream> // Prints diagnostics if shared-memory setup fails.
#include <stdexcept> // Makes standard exception types available to related history operations.
#include <unistd.h> // Provides close/getuid/getpid for POSIX resource and name management.
#include <sys/mman.h> // Provides mmap/munmap and MAP_SHARED flags for shared pages.
#include <sys/stat.h> // Provides file metadata declarations used by POSIX shared-memory setup.
#include <fcntl.h> // Provides shm_open access/create flags.
#include <fstream> // Reads/writes the persistent ~/.bash_history text file.
#include <cstdlib> // Provides getenv for HOME lookup.
#include <algorithm> // Provides max/fill operations for substring search.

namespace Terminator {

    /**
     * HistoryManager Constructor
     * Sets up POSIX shared memory object (/terminator_shm_...) and attaches to it.
     * If is_parent is true, it truncates the memory to size and initializes the semaphore.
     */
    HistoryManager::HistoryManager(bool is_parent, const std::string& shm_identifier) // Set up a shared region, either creating it or attaching to it.
        : shm_fd_(-1), shared_data_(nullptr), is_creator_(is_parent) {
        
        // Construct unique POSIX shared memory name based on user ID and parent PID
        if (shm_identifier.empty()) { // Normal GUI startup requests a unique name for this user and app process.
            shm_name_ = "/terminator_shm_" + std::to_string(getuid()) + "_" + std::to_string(getpid()); // POSIX shared-memory names start with '/' and avoid collisions across users/processes.
        } else {
            shm_name_ = shm_identifier; // Tests or cooperating children may supply a known shared-memory name.
        }

        size_t alloc_size = sizeof(SharedHistoryLayout); // Allocate precisely enough bytes for the semaphore, count, and fixed history array.

        if (is_creator_) { // Only the creator initializes data; attaching twice could reset shared history and semaphore state.
            // Unlink any leftover shared memory object with the same name
            shm_unlink(shm_name_.c_str()); // Remove a stale object from an earlier crashed run before creating a fresh one.

            // Create shared memory object with read/write access
            shm_fd_ = shm_open(shm_name_.c_str(), O_CREAT | O_RDWR | O_EXCL, 0666); // Create exclusively so this process knows it owns initialization.
            if (shm_fd_ < 0) { // If exclusive creation fails, try opening/creating without O_EXCL as a fallback.
                // If it already exists, open without O_EXCL
                shm_fd_ = shm_open(shm_name_.c_str(), O_CREAT | O_RDWR, 0666);
            }

            if (shm_fd_ < 0) { // If POSIX shm_open is unavailable, use an anonymous mapping inherited across fork.
                // Fallback to anonymous shared mapping (which fork inherits)
                shared_data_ = static_cast<SharedHistoryLayout*>(
                    mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0) // nullptr asks the kernel to choose the address; -1 means no backing file.
                );
            } else {
                // Configure memory size
                if (ftruncate(shm_fd_, alloc_size) == -1) { // A newly opened shm object has size zero until explicitly extended.
                    perror("ftruncate shm");
                }

                // Map into process virtual memory space
                shared_data_ = static_cast<SharedHistoryLayout*>(
                    mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0) // Map read/write pages shared by all processes using this shm descriptor.
                );
            }

            if (shared_data_ && shared_data_ != MAP_FAILED) { // MAP_FAILED is a sentinel pointer; null is also rejected defensively.
                // Initialize POSIX process-shared semaphore (pshared = 1, initial value = 1)
                // This guarantees mutual exclusion across all threads and processes.
                if (sem_init(&shared_data_->lock, 1, 1) != 0) { // pshared=1 makes the semaphore usable across processes; value 1 starts it unlocked.
                    perror("sem_init");
                }
                shared_data_->count = 0; // No shared entries are valid until copied from the user's persistent history file.
                const char* home = getenv("HOME"); // HOME chooses which user's history file to load.
                std::string history_path = std::string(home ? home : ".") + "/.bash_history"; // Use current-directory fallback if HOME is absent.
                std::ifstream history_file(history_path); // Failure to open is allowed; the shell then starts with empty history.
                std::string line; // getline uses this buffer for each persistent command line.
                bool history_truncated = false; // Track whether old file records exceeded the in-memory limit.
                while (std::getline(history_file, line)) { // Read one command per file line in chronological order.
                    if (line.empty()) continue; // Empty file lines are not stored as commands.
                    size_t index = shared_data_->count; // Next unused shared slot when the capacity has not been reached.
                    if (index == MAX_HISTORY_ENTRIES) { // Keep only the newest MAX_HISTORY_ENTRIES records.
                        history_truncated = true;
                        memmove(shared_data_->entries, shared_data_->entries + 1, // Shift all later records one slot toward the beginning.
                                (MAX_HISTORY_ENTRIES - 1) * MAX_HISTORY_LINE_LEN); // Byte count excludes the dropped oldest slot.
                        index = MAX_HISTORY_ENTRIES - 1; // Reuse the final slot for this newest line.
                    } else {
                        ++shared_data_->count; // The previously unused slot becomes a valid entry.
                    }
                    strncpy(shared_data_->entries[index], line.c_str(), MAX_HISTORY_LINE_LEN - 1); // Copy bounded text into the fixed-size shared slot.
                    shared_data_->entries[index][MAX_HISTORY_LINE_LEN - 1] = '\0'; // Force termination even if the source line was too long.
                }
                history_file.close(); // Release the input file descriptor before potentially replacing the file.
                if (history_truncated) { // Keep disk history consistent with the in-memory 10,000-entry limit.
                    std::string temp_path = history_path + ".terminator.tmp";
                    std::ofstream trimmed_file(temp_path, std::ios::trunc); // Write a replacement separately to avoid partial truncation of the original.
                    if (trimmed_file) { // Only write when the temporary file opened successfully.
                        for (size_t i = 0; i < shared_data_->count; ++i) trimmed_file << shared_data_->entries[i] << '\n'; // Preserve chronological order.
                        trimmed_file.close(); // Flush and close before atomic rename.
                        if (trimmed_file) rename(temp_path.c_str(), history_path.c_str()); // Replace the old file only after successful output.
                    }
                }
            } else {
                std::cerr << "Terminator Error: Failed to map shared history memory.\n"; // Mapping failure disables history and explains the problem to the launcher.
            }
        } else {
            // Child process connecting to existing shared memory
            shm_fd_ = shm_open(shm_name_.c_str(), O_RDWR, 0666); // Open the existing shared region without truncating or reinitializing it.
            if (shm_fd_ >= 0) { // Only attempt mmap after shm_open returned a valid descriptor.
                shared_data_ = static_cast<SharedHistoryLayout*>(
                    mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0) // Attach to the same bytes so semaphore and entries are visible to this process.
                );
            }
            // Process-mode tab children inherit the already initialized mapping across fork.
            // If attached independently, the semaphore/data are initialized by the parent.
        }
    }

    /**
     * HistoryManager Destructor
     * Cleans up mapped memory and removes the POSIX shm file if creator.
     */
    HistoryManager::~HistoryManager() { // Release this manager's mapping and descriptor without invalidating other attachments early.
        if (shared_data_ && shared_data_ != MAP_FAILED) { // Only unmap a pointer returned successfully by mmap.
            if (is_creator_) { // Only the original initializer should destroy the process-shared semaphore.
                // Destroy semaphore upon termination of parent
                sem_destroy(&shared_data_->lock);
            }
            munmap(shared_data_, sizeof(SharedHistoryLayout)); // Detach this process's virtual mapping; other mappings remain valid.
        }

        if (shm_fd_ >= 0) { // A valid shm descriptor was opened during construction.
            close(shm_fd_); // Release this process's file descriptor reference.
        }

        if (is_creator_ && !shm_name_.empty()) { // Only creator removes the name after users of this manager are done.
            // Remove shared memory entry from /dev/shm
            shm_unlink(shm_name_.c_str()); // Remove the name from /dev/shm; already mapped users retain their mappings.
        }
    }

    /**
     * Append a command string to shared history.
     */
    void HistoryManager::add(const std::string& command) { // Append one command while keeping the memory and file copies synchronized.
        if (!shared_data_ || shared_data_ == MAP_FAILED || command.empty()) return; // Ignore invalid storage and empty commands.

        // Acquire lock
        sem_wait(&shared_data_->lock); // Decrement the shared semaphore; if another tab owns it, block until released.

        // Avoid adding duplicate identical consecutive commands
        if (shared_data_->count > 0) { // A last-entry check avoids repeated identical commands cluttering history.
            size_t last_idx = shared_data_->count - 1; // Convert the number of entries into a zero-based last index.
            if (last_idx >= MAX_HISTORY_ENTRIES) last_idx = MAX_HISTORY_ENTRIES - 1; // Guard against a corrupted/outdated count.
            if (command == shared_data_->entries[last_idx]) { // Compare with the newest stored command.
                sem_post(&shared_data_->lock); // Always release the semaphore before returning from this early exit.
                return;
            }
        }

        bool was_full = shared_data_->count >= MAX_HISTORY_ENTRIES; // Decide between using a free slot and evicting the oldest record.
        if (!was_full) { // There is an unused shared-array slot available.
            strncpy(shared_data_->entries[shared_data_->count], command.c_str(), MAX_HISTORY_LINE_LEN - 1);
            shared_data_->entries[shared_data_->count][MAX_HISTORY_LINE_LEN - 1] = '\0';
            shared_data_->count++; // Increase the valid-entry count only after filling the slot.
        } else {
            // Buffer is full: shift left by 1 and insert at the end
            for (size_t i = 1; i < MAX_HISTORY_ENTRIES; ++i) { // Move each later record one position left to evict entry zero.
                strncpy(shared_data_->entries[i - 1], shared_data_->entries[i], MAX_HISTORY_LINE_LEN); // Copy a whole fixed-size record.
            }
            strncpy(shared_data_->entries[MAX_HISTORY_ENTRIES - 1], command.c_str(), MAX_HISTORY_LINE_LEN - 1); // Place the new command in the last slot.
            shared_data_->entries[MAX_HISTORY_ENTRIES - 1][MAX_HISTORY_LINE_LEN - 1] = '\0'; // Ensure the fixed-size record always ends with NUL.
        }

        const char* home = getenv("HOME"); // Resolve persistence path for the current user's home directory.
        std::string history_path = std::string(home ? home : ".") + "/.bash_history"; // Use relative fallback if HOME is absent.
        if (was_full) { // Once old lines are shifted out, rewrite the file to remove its oldest line too.
            std::string temp_path = history_path + ".terminator.tmp";
            std::ofstream file(temp_path, std::ios::trunc); // Write the complete bounded history to a temporary file.
            if (file) { // Do not replace the old history if temporary-file creation failed.
                for (size_t i = 0; i < MAX_HISTORY_ENTRIES; ++i) file << shared_data_->entries[i] << '\n'; // Emit records in oldest-to-newest order.
                file.close(); // Flush buffered bytes before checking the stream state.
                if (file) rename(temp_path.c_str(), history_path.c_str()); // Atomically replace the persistent file on success.
            }
        } else {
            std::ofstream file(history_path, std::ios::app); // The usual case only appends, avoiding a full file rewrite per command.
            if (file) file << shared_data_->entries[shared_data_->count - 1] << '\n'; // Persist the new last record.
        }

        // Release lock
        sem_post(&shared_data_->lock); // Increment the semaphore so another thread/process may safely access history.
    }

    /**
     * Retrieve all history entries in order.
     */
    std::vector<std::string> HistoryManager::getAll() { // Take a consistent snapshot for search, history output, or GUI navigation.
        std::vector<std::string> result; // Empty is also the safe result when history storage is unavailable.
        if (!shared_data_ || shared_data_ == MAP_FAILED) return result; // Avoid dereferencing an invalid mapping.

        sem_wait(&shared_data_->lock); // Exclude concurrent writers while copying the fixed-array contents.
        size_t count = shared_data_->count; // Snapshot the count under the same lock as the entries.
        if (count > MAX_HISTORY_ENTRIES) count = MAX_HISTORY_ENTRIES; // Clamp against malformed shared state before indexing.

        result.reserve(count); // Avoid repeated vector reallocations while copying known-size history.
        for (size_t i = 0; i < count; ++i) { // Copy entries in chronological order.
            result.emplace_back(shared_data_->entries[i]); // Construct an owned std::string from this NUL-terminated slot.
        }
        sem_post(&shared_data_->lock); // Unlock before returning the independent vector to the caller.

        return result;
    }

    std::vector<std::string> HistoryManager::search(const std::string& term) { // Search a stable snapshot without holding the shared semaphore during computation.
        std::vector<std::string> entries = getAll(); // Copy under lock, then do CPU work on private memory.
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) // Reverse order means the first exact result is the newest.
            if (*it == term) return {*it}; // Exact matches outrank partial substring matches.

        size_t best = 0; // Length of the longest common contiguous substring found so far.
        std::vector<std::string> matches; // Commands tied at that current best length.
        for (const auto& entry : entries) { // Compare the term against each historical command.
            std::vector<size_t> previous(term.size() + 1, 0), current(term.size() + 1, 0); // DP rows: cell j is the matching suffix length ending at this character.
            size_t longest = 0; // Best substring length for this one history entry.
            for (char c : entry) { // Treat each command character as the end of a candidate substring.
                for (size_t j = 1; j <= term.size(); ++j) { // Compare it with every nonempty prefix ending in the search term.
                    current[j] = c == term[j - 1] ? previous[j - 1] + 1 : 0; // Extend equal neighboring chars, otherwise reset the contiguous run.
                    longest = std::max(longest, current[j]); // Retain this command's longest matching substring.
                }
                std::swap(previous, current); // Make this row the predecessor for the next command character.
                std::fill(current.begin(), current.end(), 0); // Clear reused cells so stale lengths do not affect the next row.
            }
            if (longest > best) { best = longest; matches.clear(); } // A stronger match replaces all previous ties.
            if (longest == best && longest > 2) matches.push_back(entry); // Keep every command tied at a qualifying length.
        }
        if (best <= 2) return {}; // Requirements suppress weak matches of two characters or fewer.
        return matches; // Return all best-scoring commands in their history order.
    }

    /**
     * Retrieve a specific history item by index.
     */
    std::string HistoryManager::get(size_t index) {
        std::string entry; // Empty string is used when the requested position is absent.
        if (!shared_data_ || shared_data_ == MAP_FAILED) return entry; // Do not access unavailable shared storage.

        sem_wait(&shared_data_->lock); // Hold the lock while checking the count and copying the slot.
        if (index < shared_data_->count && index < MAX_HISTORY_ENTRIES) { // Both logical count and physical array bound must be satisfied.
            entry = shared_data_->entries[index]; // Copy the shared C string into local C++ string storage.
        }
        sem_post(&shared_data_->lock); // Release the shared lock on both valid and invalid-index paths.

        return entry; // Return the copy after no shared memory remains locked.
    }

    /**
     * Return total number of stored history entries.
     */
    size_t HistoryManager::size() {
        if (!shared_data_ || shared_data_ == MAP_FAILED) return 0; // Missing storage is equivalent to empty history.

        sem_wait(&shared_data_->lock); // Prevent a simultaneous append/eviction while reading count.
        size_t count = shared_data_->count; // Copy the count while protected by the semaphore.
        sem_post(&shared_data_->lock); // Unlock immediately because the returned scalar is an independent copy.

        return count; // Report the number of currently valid entries.
    }

} // namespace Terminator

// EOF
