#include "history.hpp"
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

namespace Terminator {

    /**
     * HistoryManager Constructor
     * Sets up POSIX shared memory object (/terminator_shm_...) and attaches to it.
     * If is_parent is true, it truncates the memory to size and initializes the semaphore.
     */
    HistoryManager::HistoryManager(bool is_parent, const std::string& shm_identifier)
        : shm_fd_(-1), shared_data_(nullptr), is_creator_(is_parent) {
        
        // Construct unique POSIX shared memory name based on user ID and parent PID
        if (shm_identifier.empty()) {
            shm_name_ = "/terminator_shm_" + std::to_string(getuid()) + "_" + std::to_string(getpid());
        } else {
            shm_name_ = shm_identifier;
        }

        size_t alloc_size = sizeof(SharedHistoryLayout);

        if (is_creator_) {
            // Unlink any leftover shared memory object with the same name
            shm_unlink(shm_name_.c_str());

            // Create shared memory object with read/write access
            shm_fd_ = shm_open(shm_name_.c_str(), O_CREAT | O_RDWR | O_EXCL, 0666);
            if (shm_fd_ < 0) {
                // If it already exists, open without O_EXCL
                shm_fd_ = shm_open(shm_name_.c_str(), O_CREAT | O_RDWR, 0666);
            }

            if (shm_fd_ < 0) {
                // Fallback to anonymous shared mapping (which fork inherits)
                shared_data_ = static_cast<SharedHistoryLayout*>(
                    mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0)
                );
            } else {
                // Configure memory size
                if (ftruncate(shm_fd_, alloc_size) == -1) {
                    perror("ftruncate shm");
                }

                // Map into process virtual memory space
                shared_data_ = static_cast<SharedHistoryLayout*>(
                    mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0)
                );
            }

            if (shared_data_ && shared_data_ != MAP_FAILED) {
                // Initialize POSIX process-shared semaphore (pshared = 1, initial value = 1)
                // This guarantees mutual exclusion across all threads and processes.
                if (sem_init(&shared_data_->lock, 1, 1) != 0) {
                    perror("sem_init");
                }
                shared_data_->count = 0;
            } else {
                std::cerr << "Terminator Error: Failed to map shared history memory.\n";
            }
        } else {
            // Child process connecting to existing shared memory
            shm_fd_ = shm_open(shm_name_.c_str(), O_RDWR, 0666);
            if (shm_fd_ >= 0) {
                shared_data_ = static_cast<SharedHistoryLayout*>(
                    mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0)
                );
            }
        }
    }

    /**
     * HistoryManager Destructor
     * Cleans up mapped memory and removes the POSIX shm file if creator.
     */
    HistoryManager::~HistoryManager() {
        if (shared_data_ && shared_data_ != MAP_FAILED) {
            if (is_creator_) {
                // Destroy semaphore upon termination of parent
                sem_destroy(&shared_data_->lock);
            }
            munmap(shared_data_, sizeof(SharedHistoryLayout));
        }

        if (shm_fd_ >= 0) {
            close(shm_fd_);
        }

        if (is_creator_ && !shm_name_.empty()) {
            // Remove shared memory entry from /dev/shm
            shm_unlink(shm_name_.c_str());
        }
    }

    /**
     * Append a command string to shared history.
     */
    void HistoryManager::add(const std::string& command) {
        if (!shared_data_ || shared_data_ == MAP_FAILED || command.empty()) return;

        // Acquire lock
        sem_wait(&shared_data_->lock);

        // Avoid adding duplicate identical consecutive commands
        if (shared_data_->count > 0) {
            size_t last_idx = shared_data_->count - 1;
            if (last_idx >= MAX_HISTORY_ENTRIES) last_idx = MAX_HISTORY_ENTRIES - 1;
            if (command == shared_data_->entries[last_idx]) {
                sem_post(&shared_data_->lock);
                return;
            }
        }

        if (shared_data_->count < MAX_HISTORY_ENTRIES) {
            strncpy(shared_data_->entries[shared_data_->count], command.c_str(), MAX_HISTORY_LINE_LEN - 1);
            shared_data_->entries[shared_data_->count][MAX_HISTORY_LINE_LEN - 1] = '\0';
            shared_data_->count++;
        } else {
            // Buffer is full: shift left by 1 and insert at the end
            for (size_t i = 1; i < MAX_HISTORY_ENTRIES; ++i) {
                strncpy(shared_data_->entries[i - 1], shared_data_->entries[i], MAX_HISTORY_LINE_LEN);
            }
            strncpy(shared_data_->entries[MAX_HISTORY_ENTRIES - 1], command.c_str(), MAX_HISTORY_LINE_LEN - 1);
            shared_data_->entries[MAX_HISTORY_ENTRIES - 1][MAX_HISTORY_LINE_LEN - 1] = '\0';
        }

        // Release lock
        sem_post(&shared_data_->lock);
    }

    /**
     * Retrieve all history entries in order.
     */
    std::vector<std::string> HistoryManager::getAll() {
        std::vector<std::string> result;
        if (!shared_data_ || shared_data_ == MAP_FAILED) return result;

        sem_wait(&shared_data_->lock);
        size_t count = shared_data_->count;
        if (count > MAX_HISTORY_ENTRIES) count = MAX_HISTORY_ENTRIES;

        result.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            result.emplace_back(shared_data_->entries[i]);
        }
        sem_post(&shared_data_->lock);

        return result;
    }

    /**
     * Retrieve a specific history item by index.
     */
    std::string HistoryManager::get(size_t index) {
        std::string entry;
        if (!shared_data_ || shared_data_ == MAP_FAILED) return entry;

        sem_wait(&shared_data_->lock);
        if (index < shared_data_->count && index < MAX_HISTORY_ENTRIES) {
            entry = shared_data_->entries[index];
        }
        sem_post(&shared_data_->lock);

        return entry;
    }

    /**
     * Return total number of stored history entries.
     */
    size_t HistoryManager::size() {
        if (!shared_data_ || shared_data_ == MAP_FAILED) return 0;

        sem_wait(&shared_data_->lock);
        size_t count = shared_data_->count;
        sem_post(&shared_data_->lock);

        return count;
    }

} // namespace Terminator

