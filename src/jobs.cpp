#include "jobs.hpp"
#include <sstream>
#include <algorithm>
#include <csignal>
#include <sys/wait.h>

namespace Terminator {

    JobManager::JobManager() : next_job_id_(1) {}

    int JobManager::addJob(pid_t pgid, const std::vector<pid_t>& pids, const std::string& command, JobState state) {
        Job job;
        job.id = next_job_id_++;
        job.pgid = pgid;
        job.pids = pids;
        job.command = command;
        job.state = state;
        job.notified = false;

        jobs_.push_back(job);
        return job.id;
    }

    void JobManager::removeJob(int id) {
        jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
            [id](const Job& j) { return j.id == id; }), jobs_.end());
    }

    Job* JobManager::getJob(int id) {
        for (auto& j : jobs_) {
            if (j.id == id) return &j;
        }
        return nullptr;
    }

    Job* JobManager::getLatestJob() {
        if (jobs_.empty()) return nullptr;
        return &jobs_.back();
    }

    Job* JobManager::findJobBySpec(const std::string& spec) {
        if (jobs_.empty()) return nullptr;

        std::string s = spec;
        if (!s.empty() && s[0] == '%') {
            s = s.substr(1);
        }

        if (s.empty() || s == "+" || s == "%") {
            return getLatestJob();
        }

        if (s == "-") {
            if (jobs_.size() >= 2) {
                return &jobs_[jobs_.size() - 2];
            }
            return getLatestJob();
        }

        try {
            int id = std::stoi(s);
            return getJob(id);
        } catch (...) {
            // Check by command prefix match
            for (auto it = jobs_.rbegin(); it != jobs_.rend(); ++it) {
                if (it->command.rfind(s, 0) == 0) {
                    return &(*it);
                }
            }
        }

        return nullptr;
    }

    void JobManager::updateStatus(std::string& out_messages) {
        std::ostringstream oss;

        for (auto& job : jobs_) {
            bool all_done = true;
            bool any_stopped = false;

            for (pid_t pid : job.pids) {
                int status;
                pid_t res = waitpid(pid, &status, WNOHANG | WUNTRACED | WCONTINUED);

                if (res == 0) {
                    // Child is still running
                    all_done = false;
                } else if (res > 0) {
                    if (WIFSTOPPED(status)) {
                        any_stopped = true;
                        all_done = false;
                    } else if (WIFCONTINUED(status)) {
                        job.state = JobState::RUNNING;
                        all_done = false;
                    }
                    // If WIFEXITED or WIFSIGNALED, this pid is done
                }
            }

            if (any_stopped && job.state != JobState::STOPPED) {
                job.state = JobState::STOPPED;
                oss << "[" << job.id << "]+ Stopped " << job.command << "\n";
            } else if (all_done && job.state != JobState::COMPLETED) {
                job.state = JobState::COMPLETED;
                oss << "[" << job.id << "]+ Done " << job.command << "\n";
            }
        }

        out_messages += oss.str();
        purgeCompleted();
    }

    void JobManager::purgeCompleted() {
        jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
            [](const Job& j) { return j.state == JobState::COMPLETED; }), jobs_.end());
    }

    std::string JobManager::listJobs() {
        std::ostringstream oss;
        for (size_t i = 0; i < jobs_.size(); ++i) {
            const auto& j = jobs_[i];
            char marker = (i == jobs_.size() - 1) ? '+' : (i == jobs_.size() - 2 ? '-' : ' ');
            std::string state_str;
            switch (j.state) {
                case JobState::RUNNING: state_str = "Running"; break;
                case JobState::STOPPED: state_str = "Stopped"; break;
                case JobState::COMPLETED: state_str = "Done"; break;
            }

            oss << "[" << j.id << "]" << marker << " " << state_str << " " << j.command;
            if (j.state == JobState::RUNNING) {
                oss << " &";
            }
            oss << "\n";
        }
        return oss.str();
    }

    void JobManager::terminateAll() {
        for (const auto& job : jobs_) {
            if (job.pgid > 0) {
                kill(-job.pgid, SIGTERM);
                kill(-job.pgid, SIGCONT);
            }
        }
        jobs_.clear();
    }

} // namespace Terminator

