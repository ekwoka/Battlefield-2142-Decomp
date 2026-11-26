// enhanced.botAsyncPaths: a few lower-priority threads that make the bots'
// costly navigation (a Titan's fields) off the main thread. Not retail: the
// original's AI runs on the main thread.
//
// A job reads only what it was handed (copies, or data that nobody changes
// while it runs) and writes only its own result; the bots take the result at
// a fixed later tick and wait for it then if it is late, so a round plays the
// same whatever the threads' timing (and the same with no threads at all,
// when the job runs where it is waited for).
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace dice::hfe::game {

class BotWorkers {
public:
	struct Job {
		std::function<void()> run;
		std::atomic<int> state {0}; // 0 queued, 1 running, 2 done
		std::mutex mutex;
		std::condition_variable doneSignal;
	};

	// `threads` 0: every job runs in wait().
	explicit BotWorkers(int threads);
	~BotWorkers();
	BotWorkers(const BotWorkers&) = delete;
	BotWorkers& operator=(const BotWorkers&) = delete;

	std::shared_ptr<Job> post(std::function<void()> run);
	// Until the job is done: run here if no thread has taken it yet.
	void wait(const std::shared_ptr<Job>& job);
	int threads() const { return static_cast<int>(m_threads.size()); }
	// The default for this machine: one thread for every four cores, one or two.
	static int defaultThreads();

private:
	static void execute(Job& job);
	void work();
	std::vector<std::thread> m_threads;
	std::deque<std::shared_ptr<Job>> m_queue;
	std::mutex m_mutex;
	std::condition_variable m_wake;
	bool m_stop = false;
};

} // namespace dice::hfe::game
