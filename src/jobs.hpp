#ifndef TERMINATOR_JOBS_HPP // Prevent duplicate declarations if shell and tests both include this file.
#define TERMINATOR_JOBS_HPP // Set the unique include-guard symbol.

#include "common.hpp" // Supplies pid_t, waitpid declarations, and shared POSIX utilities.
#include <string> // Stores the original command text for job listings and notifications.
#include <vector> // Tracks each child PID that belongs to a pipeline job.
#include <memory> // Included for job-related ownership utilities used by consumers.

namespace Terminator {

    enum class JobState { // Shell-level states derived from waitpid status reports.
        RUNNING, // At least one process in the group is executing or resumed in the background.
        STOPPED, // The process group has been suspended, typically by SIGTSTP/Ctrl+Z.
        COMPLETED // Every process in the job has exited or been reaped.
    };

    /**
     * Represents a background or stopped process / pipeline job in a Tab.
     */
    struct Job { // Metadata for one background or suspended pipeline in a particular tab shell.
        int id; // User-facing job number such as %1; separate from operating-system PIDs.
        pid_t pgid; // Process-group ID used to signal the entire pipeline with kill(-pgid, signal).
        std::vector<pid_t> pids; // Individual child IDs required to reap every process in the pipeline.
        std::string command; // Original text shown by jobs/fg/bg and completion notifications.
        JobState state; // Most recently observed aggregate job state.
        bool notified; // Prevents a completed-job notice from being printed more than once.
    };

    /**
     * JobManager tracks and manages active background and suspended jobs
     * for a specific shell tab.
     */
    class JobManager { // Maintains one tab's job table and translates wait statuses into shell job states.
    public:
        JobManager(); // Starts job numbering at the first available job ID.

        /**
         * Add a new job.
         * @return Allocated job ID.
         */
        int addJob(pid_t pgid, const std::vector<pid_t>& pids, const std::string& command, JobState state); // Record a new group and return its shell job number.

        /**
         * Remove a job by ID.
         */
        void removeJob(int id); // Erase the record after its process group is no longer manageable.

        /**
         * Retrieve a job pointer by ID.
         */
        Job* getJob(int id); // Return a pointer into the table, or nullptr when the ID is absent.

        /**
         * Retrieve the most recent job (current job '+').
         */
        Job* getLatestJob(); // Find the current '+' job used when a command omits an explicit job specifier.

        /**
         * Retrieve a job by job specification (e.g. "%1", "1", "+", "-").
         */
        Job* findJobBySpec(const std::string& spec); // Resolve forms such as %2, 2, +, and - to a stored job.

        /**
         * Update status of all child processes non-blockingly using waitpid(..., WNOHANG | WUNTRACED | WCONTINUED).
         * @param out_messages Buffer to collect status change notices (e.g. "[1]+ Done sleep 10").
         */
        void updateStatus(std::string& out_messages); // Call waitpid with WNOHANG and append state-change notices without blocking.

        /**
         * List all jobs in standard format:
         * [1]+ Stopped sleep 100
         * [2]- Running python app.py &
         */
        std::string listJobs(); // Format current job IDs, current/previous markers, states, and commands.

        /**
         * Check if any jobs exist.
         */
        bool hasJobs() const { return !jobs_.empty(); } // Constant-time check used during tab shutdown.

        /**
         * Clean up completed jobs.
         */
        void purgeCompleted(); // Remove finished records after any required notification has been emitted.

        /**
         * Terminate all jobs (called when tab is closing).
         */
        void terminateAll(); // Signal remaining process groups and wait/reap their child processes at shell shutdown.

    private:
        std::vector<Job> jobs_; // Owns the current tab's job records in insertion order.
        int next_job_id_; // Monotonic next ID, so removing a job does not immediately recycle its number.
    };

} // namespace Terminator

#endif // TERMINATOR_JOBS_HPP closes the header guard.

// EOF
