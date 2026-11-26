#include "BotWorkers.h"

#include <algorithm>

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sys/resource.h>
#endif

namespace dice::hfe::game {

namespace {

// Below the game's own threads: the main thread's tick comes first.
void lowerPriority()
{
#if defined(__APPLE__)
	pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#elif defined(_WIN32)
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__linux__)
	setpriority(PRIO_PROCESS, 0, 5); // Linux: the calling thread's niceness
#endif
}

} // namespace

BotWorkers::BotWorkers(int threads)
{
	for (int i = 0; i < threads; ++i) {
		m_threads.emplace_back([this] { work(); });
	}
}

BotWorkers::~BotWorkers()
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_stop = true;
	}
	m_wake.notify_all();
	for (std::thread& thread : m_threads) {
		thread.join();
	}
}

int BotWorkers::defaultThreads()
{
	const unsigned cores = std::thread::hardware_concurrency();
	return std::clamp(static_cast<int>(cores / 4), 1, 2);
}

std::shared_ptr<BotWorkers::Job> BotWorkers::post(std::function<void()> run)
{
	auto job = std::make_shared<Job>();
	job->run = std::move(run);
	if (!m_threads.empty()) {
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_queue.push_back(job);
		}
		m_wake.notify_one();
	}
	return job;
}

void BotWorkers::execute(Job& job)
{
	job.run();
	job.run = nullptr;
	{
		std::lock_guard<std::mutex> lock(job.mutex);
		job.state = 2;
	}
	job.doneSignal.notify_all();
}

void BotWorkers::wait(const std::shared_ptr<Job>& job)
{
	if (!job) {
		return;
	}
	int queued = 0;
	if (job->state.compare_exchange_strong(queued, 1)) {
		execute(*job); // (a thread popping it later finds it taken)
		return;
	}
	std::unique_lock<std::mutex> lock(job->mutex);
	job->doneSignal.wait(lock, [&] { return job->state == 2; });
}

void BotWorkers::work()
{
	lowerPriority();
	for (;;) {
		std::shared_ptr<Job> job;
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_wake.wait(lock, [&] { return m_stop || !m_queue.empty(); });
			if (m_stop) {
				return;
			}
			job = std::move(m_queue.front());
			m_queue.pop_front();
		}
		int queued = 0;
		if (job->state.compare_exchange_strong(queued, 1)) {
			execute(*job);
		}
	}
}

} // namespace dice::hfe::game
