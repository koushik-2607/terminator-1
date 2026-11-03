#ifndef TERMINATOR_JOBS_HPP
#define TERMINATOR_JOBS_HPP

#include "common.hpp"
#include <string>
#include <vector>
#include <memory>

namespace Terminator {

    enum class JobState {
        RUNNING,
        STOPPED,
        COMPLETED
    };

    /**
     * Represents a background or stopped process / pipeline job in a Tab.
     */
    struct Job {
        int id;                     // Job number (1, 2, 3...)
        pid_t pgid;                 // Process Group ID
        std::vector<pid_t> pids;    // PIDs of processes in this job pipeline
        std::string command;        // Command text
        JobState state;             // Current state
        bool notified;              // Whether completion has been reported to user
    };

    /**
     * JobManager tracks and manages active background and suspended jobs
     * for a specific shell tab.
     */
    class JobManager {
    public:
        JobManager();

        /**
         * Add a new job.
         * @return Allocated job ID.
         */
        int addJob(pid_t pgid, const std::vector<pid_t>& pids, const std::string& command, JobState state);

        /**
         * Remove a job by ID.
         */
        void removeJob(int id);

        /**
         * Retrieve a job pointer by ID.
         */
        Job* getJob(int id);

        /**
         * Retrieve the most recent job (current job '+').
         */
        Job* getLatestJob();

        /**
         * Retrieve a job by job specification (e.g. "%1", "1", "+", "-").
         */
        Job* findJobBySpec(const std::string& spec);

        /**
         * Update status of all child processes non-blockingly using waitpid(..., WNOHANG | WUNTRACED | WCONTINUED).
         * @param out_messages Buffer to collect status change notices (e.g. "[1]+ Done sleep 10").
         */
        void updateStatus(std::string& out_messages);

        /**
         * List all jobs in standard format:
         * [1]+ Stopped sleep 100
         * [2]- Running python app.py &
         */
        std::string listJobs();

        /**
         * Check if any jobs exist.
         */
        bool hasJobs() const { return !jobs_.empty(); }

        /**
         * Clean up completed jobs.
         */
        void purgeCompleted();

        /**
         * Terminate all jobs (called when tab is closing).
         */
        void terminateAll();

    private:
        std::vector<Job> jobs_;
        int next_job_id_;
    };

} // namespace Terminator

#endif // TERMINATOR_JOBS_HPP

