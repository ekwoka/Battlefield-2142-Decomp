#include "TestMain.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

// A child's exit status for a test that skipped itself.
constexpr int kSkippedStatus = 77;

enum class Result { Pass, Fail, Skip };

// Each test's time in this run (not the skipped ones), for the times file.
std::map<std::string, double>& measured()
{
	static std::map<std::string, double> times;
	return times;
}

double secondsSince(Clock::time_point start)
{
	return std::chrono::duration<double>(Clock::now() - start).count();
}

// One test's result line; the time only for tests worth noticing.
void report(Result result, const char* name, double seconds)
{
	const char* word = result == Result::Pass ? "PASS" : result == Result::Fail ? "FAIL" : "SKIP";
	if (result != Result::Skip) {
		measured()[name] = seconds;
	}
	if (seconds >= 1.0) {
		std::printf("%s %s (%.1f s)\n", word, name, seconds);
	} else {
		std::printf("%s %s\n", word, name);
	}
	std::fflush(stdout);
}

// The tests' last times, "<seconds> <name>" a line: BF2142_TEST_TIMES, else
// test-times.txt beside the test binary (in the build directory).
std::string timesPath(const char* argv0)
{
	if (const char* path = std::getenv("BF2142_TEST_TIMES"); path && *path) {
		return path;
	}
	return (std::filesystem::path(argv0).parent_path() / "test-times.txt").string();
}

std::map<std::string, double> loadTimes(const std::string& path)
{
	std::map<std::string, double> times;
	std::ifstream in(path);
	double seconds = 0;
	std::string name;
	while (in >> seconds >> name) {
		times[name] = seconds;
	}
	return times;
}

// This run's times over the file's (the tests not run keep theirs).
void saveTimes(const std::string& path, std::map<std::string, double> times)
{
	for (const auto& [name, seconds] : measured()) {
		times[name] = seconds;
	}
	std::ofstream out(path);
	for (const auto& [name, seconds] : times) {
		out << seconds << ' ' << name << '\n';
	}
}

// The default for `-j`: every core when the run is niced to 10 or more (`nice`
// alone gives 10), since it then yields to everything else on the machine;
// otherwise at most 8, leaving the rest free.
int defaultJobs()
{
	const int cores = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
#ifndef _WIN32
	errno = 0;
	const int niceness = getpriority(PRIO_PROCESS, 0);
	if (errno == 0 && niceness >= 10) {
		return cores;
	}
#endif
	return std::min(cores, 8);
}

#ifndef _WIN32
// Runs the tests in up to `jobs` child processes at once, each test's output
// kept in a temporary file and printed whole when it ends.
int runParallel(const std::vector<const test::Case*>& cases, int jobs, int& skipped)
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
				_exit(test::failures() != 0 ? 1 : test::skipped() ? kSkippedStatus : 0);
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
			const bool skip = WIFEXITED(status) && WEXITSTATUS(status) == kSkippedStatus;
			const bool passed = skip || (WIFEXITED(status) && WEXITSTATUS(status) == 0);
			if (!passed && !WIFEXITED(status)) {
				std::printf("%s: crashed (signal %d)\n", r.c->name, WIFSIGNALED(status) ? WTERMSIG(status) : 0);
			}
			failed += passed ? 0 : 1;
			skipped += skip ? 1 : 0;
			report(skip ? Result::Skip : passed ? Result::Pass : Result::Fail, r.c->name, secondsSince(r.start));
			break;
		}
	}
	return failed;
}
#endif

} // namespace

// Runs every test, or those whose names contain the filter. Outside
// Windows the tests run in their own processes, as many at once as there are
// cores when niced to 10 or more (`nice bf2142_tests`), else at most 8; `-j N`
// sets how many, `-j 1` runs them one after another in this one.
// `--remaster` (or BF2142_REMASTER_TESTS=1) adds the Project Remaster tests.
int main(int argc, char** argv)
{
	const char* filter = nullptr;
	int jobs = defaultJobs();
	const char* remasterEnv = std::getenv("BF2142_REMASTER_TESTS");
	test::remaster() = remasterEnv && *remasterEnv && std::strcmp(remasterEnv, "0") != 0;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "-j") == 0 && i + 1 < argc) {
			jobs = std::max(1, std::atoi(argv[++i]));
		} else if (std::strcmp(argv[i], "--remaster") == 0) {
			test::remaster() = true;
		} else {
			filter = argv[i];
		}
	}
	if (test::remaster()) {
		const char* gameDir = std::getenv("BF2142_GAME_DIR");
		const std::string mod = std::string(gameDir ? gameDir : "") + "/mods/Project_Remaster_v17_Final";
		std::error_code error;
		if (!gameDir || !std::filesystem::is_directory(mod, error)) {
			std::fprintf(stderr, "--remaster: Project Remaster not found at %s\n", mod.c_str());
			return 2;
		}
	}
	std::vector<const test::Case*> cases;
	for (const test::Case& c : test::registry()) {
		if (!filter || std::strstr(c.name, filter)) {
			cases.push_back(&c);
		}
	}

	// Slowest first, by the last run's times (a test with none counts as the
	// slowest: new ones start early): with N jobs the N slowest each start on
	// a process of their own at once and the quick ones share the rest, so
	// the run isn't left waiting on a long test that started last.
	const std::string timesFile = timesPath(argv[0]);
	const std::map<std::string, double> lastTimes = loadTimes(timesFile);
	if (jobs > 1) {
		const auto cost = [&lastTimes](const test::Case* c) {
			const auto it = lastTimes.find(c->name);
			return it == lastTimes.end() ? 1e9 : it->second;
		};
		std::stable_sort(cases.begin(), cases.end(), [&cost](const test::Case* a, const test::Case* b) { return cost(a) > cost(b); });
	}

	const Clock::time_point start = Clock::now();
	int failed = 0;
	int skipped = 0;
#ifndef _WIN32
	if (jobs > 1 && cases.size() > 1) {
		failed = runParallel(cases, jobs, skipped);
	} else
#endif
	{
		for (const test::Case* c : cases) {
			const int before = test::failures();
			const Clock::time_point testStart = Clock::now();
			test::skipped() = false;
			c->body();
			const bool passed = test::failures() == before;
			const bool skip = passed && test::skipped();
			failed += passed ? 0 : 1;
			skipped += skip ? 1 : 0;
			report(skip ? Result::Skip : passed ? Result::Pass : Result::Fail, c->name, secondsSince(testStart));
		}
	}
	saveTimes(timesFile, lastTimes);
	std::printf("%zu tests, %d failed, %d skipped, %.1f s\n", cases.size(), failed, skipped, secondsSince(start));
	return failed == 0 ? 0 : 1;
}
