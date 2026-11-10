#include "jobs.hpp" // Declares job metadata and manager operations.
#include <sstream> // Formats the user-facing job table and status notices.
#include <algorithm> // Supplies remove_if for deleting records from vectors.
#include <csignal> // Declares SIGTERM and SIGCONT used during tab shutdown.
#include <sys/wait.h> // Provides waitpid and WIF*/WNOHANG status macros.

namespace Terminator {

    JobManager::JobManager() : next_job_id_(1) {} // Begin visible shell job numbering at one.

    int JobManager::addJob(pid_t pgid, const std::vector<pid_t>& pids, const std::string& command, JobState state) {
        Job job; // Create a local record; vector insertion below takes an owned copy.
        job.id = next_job_id_++; // Assign the current job number, then reserve the next one.
        job.pgid = pgid; // Save the group ID used to signal a pipeline as one unit.
        job.pids = pids; // Copy every process ID so updateStatus can reap each child.
        job.command = command; // Retain original command text for listing and status messages.
        job.state = state; // Start in the state provided by the launching shell.
        job.notified = false; // No completion message has been emitted for a new job.

        jobs_.push_back(job); // Add this job to the manager's owned list.
        return job.id; // Give the shell the user-visible identifier.
    }

    void JobManager::removeJob(int id) {
        jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), // Compact away records that meet the predicate.
            [id](const Job& j) { return j.id == id; }), jobs_.end()); // Match only the requested job number.
    }

    Job* JobManager::getJob(int id) {
        for (auto& j : jobs_) { // Search owned records in insertion order.
            if (j.id == id) return &j; // Return a pointer to the live vector element for direct state changes.
        }
        return nullptr; // A null pointer is the explicit not-found result.
    }

    Job* JobManager::getLatestJob() {
        if (jobs_.empty()) return nullptr; // An empty table has no current '+' job.
        return &jobs_.back(); // Most recently inserted job is treated as the current job.
    }

    Job* JobManager::findJobBySpec(const std::string& spec) {
        if (jobs_.empty()) return nullptr; // No specification can resolve when the table is empty.

        std::string s = spec; // Work on a copy so parsing does not modify the caller's job spec.
        if (!s.empty() && s[0] == '%') { // Bash-style job numbers may include a leading percent sign.
            s = s.substr(1); // Remove '%' before interpreting the remainder.
        }

        if (s.empty() || s == "+" || s == "%") {
            return getLatestJob(); // Empty, '+' and '%' all mean the current job.
        }

        if (s == "-") {
            if (jobs_.size() >= 2) { // The previous '-' job is second newest when available.
                return &jobs_[jobs_.size() - 2]; // Vector indexing returns a pointer-stable element until the vector changes.
            }
            return getLatestJob(); // With only one job, it is both current and previous fallback.
        }

        try {
            int id = std::stoi(s); // Convert a numeric string into a shell job number.
            return getJob(id); // Resolve the numeric identifier through the normal lookup function.
        } catch (...) {
            // Check by command prefix match
            for (auto it = jobs_.rbegin(); it != jobs_.rend(); ++it) { // Search newest-first if the spec is not numeric.
                if (it->command.rfind(s, 0) == 0) { // Allow a command-name prefix as a convenient job selector.
                    return &(*it); // Return the matching job record.
                }
            }
        }

        return nullptr; // No numeric ID or command prefix matched.
    }

    void JobManager::updateStatus(std::string& out_messages) {
        std::ostringstream oss; // Buffer notices so the caller receives one combined string.

        for (auto& job : jobs_) { // Refresh each tracked job from its child-process statuses.
            bool all_done = true; // Becomes false if any child is still running or stopped.
            bool any_stopped = false; // Records whether at least one child reported WIFSTOPPED.

            for (pid_t pid : job.pids) { // Wait for each child individually so every pipeline process is tracked.
                int status; // waitpid writes encoded exit/stop/continue details into this integer.
                pid_t res = waitpid(pid, &status, WNOHANG | WUNTRACED | WCONTINUED); // Never block: also request stop/continue notifications.

                if (res == 0) {
                    // Child is still running
                    all_done = false;
                } else if (res > 0) { // A positive return means a child status change was consumed.
                    if (WIFSTOPPED(status)) { // The child was suspended by a stop signal.
                        any_stopped = true; // Mark the aggregate job stopped if any stage stopped.
                        all_done = false; // A stopped child has not completed and remains resumable.
                    } else if (WIFCONTINUED(status)) { // The child resumed after SIGCONT.
                        job.state = JobState::RUNNING; // Reflect that it can execute again.
                        all_done = false; // A continued process is still active.
                    }
                    // If WIFEXITED or WIFSIGNALED, this pid is done
                }
            }

            if (any_stopped && job.state != JobState::STOPPED) { // Emit one notice when transitioning into stopped state.
                job.state = JobState::STOPPED; // Persist the observed state for jobs/fg/bg.
                oss << "[" << job.id << "]+ Stopped " << job.command << "\n"; // Build the shell-style status line.
            } else if (all_done && job.state != JobState::COMPLETED) { // Emit completion once, after every child has exited.
                job.state = JobState::COMPLETED; // Mark this record ready for purging after notification.
                oss << "[" << job.id << "]+ Done " << job.command << "\n"; // Format the completion message.
            }
        }

        out_messages += oss.str(); // Append this poll's notices to any message already assembled by the caller.
        purgeCompleted(); // Remove completed records only after their message has been copied out.
    }

    void JobManager::purgeCompleted() {
        jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), // Use erase-remove to compact the vector in one pass.
            [](const Job& j) { return j.state == JobState::COMPLETED; }), jobs_.end()); // Only completed jobs are discarded.
    }

    std::string JobManager::listJobs() {
        std::ostringstream oss; // Build the full jobs output without repeated string concatenation.
        for (size_t i = 0; i < jobs_.size(); ++i) { // Preserve the manager's job order when displaying rows.
            const auto& j = jobs_[i]; // Read the current record without copying its process vector.
            char marker = (i == jobs_.size() - 1) ? '+' : (i == jobs_.size() - 2 ? '-' : ' '); // Mark newest as current and previous as prior.
            std::string state_str; // Human-readable state label assembled from the enum below.
            switch (j.state) { // Convert internal enum values to shell-facing names.
                case JobState::RUNNING: state_str = "Running"; break;
                case JobState::STOPPED: state_str = "Stopped"; break;
                case JobState::COMPLETED: state_str = "Done"; break;
            }

            oss << "[" << j.id << "]" << marker << " " << state_str << " " << j.command; // Print job number, marker, state, and command.
            if (j.state == JobState::RUNNING) { // Bash-style listings mark active background commands with '&'.
                oss << " &"; // Append the background marker after the command.
            }
            oss << "\n"; // Finish this job's row.
        }
        return oss.str(); // Give the caller the complete formatted listing.
    }

    void JobManager::terminateAll() {
        for (const auto& job : jobs_) { // Visit every remaining job during tab teardown.
            if (job.pgid > 0) { // Negative PID targets a process group; zero/negative stored IDs are invalid here.
                kill(-job.pgid, SIGTERM); // Ask the whole job group to terminate cleanly.
                kill(-job.pgid, SIGCONT); // Resume stopped jobs so they can process termination and be reaped.
            }
        }
        jobs_.clear(); // Drop records after signals have been sent; owning shell performs remaining wait/reap work.
    }

} // namespace Terminator

// EOF
