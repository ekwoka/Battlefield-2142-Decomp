#include "TestMain.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point start)
{
	return std::chrono::duration<double>(Clock::now() - start).count();
}

// One test's result line; the time only for tests worth noticing.
void report(bool passed, const char* name, double seconds)
{
	if (seconds >= 1.0) {
		std::printf("%s %s (%.1f s)\n", passed ? "PASS" : "FAIL", name, seconds);
	} else {
		std::printf("%s %s\n", passed ? "PASS" : "FAIL", name);
	}
	std::fflush(stdout);
}

#ifndef _WIN32
// Runs the tests in up to `jobs` child processes at once, each test's output
// kept in a temporary file and printed whole when it ends.
int runParallel(const std::vector<const test::Case*>& cases, int jobs)
{
	struct Running {
		pid_t pid;
		const test::Case* c;
		std::FILE* out;
		Clock::time_point start;
	};
	std::vector<Running> running;
	size_t next = 0;
	int failed = 0;
	while (next < cases.size() || !running.empty()) {
		while (next < cases.size() && static_cast<int>(running.size()) < jobs) {
			const test::Case* c = cases[next++];
			std::FILE* out = std::tmpfile();
			std::fflush(stdout);
			std::fflush(stderr);
			const pid_t pid = fork();
			if (pid == 0) {
				dup2(fileno(out), STDOUT_FILENO);
				dup2(fileno(out), STDERR_FILENO);
				c->body();
				std::fflush(stdout);
				std::fflush(stderr);
				_exit(test::failures() == 0 ? 0 : 1);
			}
			running.push_back({pid, c, out, Clock::now()});
		}
		int status = 0;
		const pid_t done = wait(&status);
		for (size_t i = 0; i < running.size(); ++i) {
			if (running[i].pid != done) {
				continue;
			}
			Running r = running[i];
			running.erase(running.begin() + static_cast<long>(i));
			std::rewind(r.out);
			char buffer[4096];
			size_t n;
			while ((n = std::fread(buffer, 1, sizeof buffer, r.out)) > 0) {
				std::fwrite(buffer, 1, n, stdout);
			}
			std::fclose(r.out);
			const bool passed = WIFEXITED(status) && WEXITSTATUS(status) == 0;
			if (!passed && !WIFEXITED(status)) {
				std::printf("%s: crashed (signal %d)\n", r.c->name, WIFSIGNALED(status) ? WTERMSIG(status) : 0);
			}
			failed += passed ? 0 : 1;
			report(passed, r.c->name, secondsSince(r.start));
			break;
		}
	}
	return failed;
}
#endif

} // namespace

// Runs every test, or those whose names contain the filter. Outside
// Windows the tests run in their own processes, as many at once as there are
// cores; `-j N` sets how many, `-j 1` runs them one after another in this one.
int main(int argc, char** argv)
{
	const char* filter = nullptr;
	int jobs = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "-j") == 0 && i + 1 < argc) {
			jobs = std::max(1, std::atoi(argv[++i]));
		} else {
			filter = argv[i];
		}
	}
	std::vector<const test::Case*> cases;
	for (const test::Case& c : test::registry()) {
		if (!filter || std::strstr(c.name, filter)) {
			cases.push_back(&c);
		}
	}

	const Clock::time_point start = Clock::now();
	int failed = 0;
#ifndef _WIN32
	if (jobs > 1 && cases.size() > 1) {
		failed = runParallel(cases, jobs);
	} else
#endif
	{
		for (const test::Case* c : cases) {
			const int before = test::failures();
			const Clock::time_point testStart = Clock::now();
			c->body();
			const bool passed = test::failures() == before;
			failed += passed ? 0 : 1;
			report(passed, c->name, secondsSince(testStart));
		}
	}
	std::printf("%zu tests, %d failed, %.1f s\n", cases.size(), failed, secondsSince(start));
	return failed == 0 ? 0 : 1;
}
