// The console and its script language (the "MainConsole" singleton).
//
// Everything the game configures at start-up goes through console scripts:
// .con files (run with "run", own scope) and .inc/.tweak files (pulled in
// with "include", caller's scope). A line is either a keyword or a call
// "object.method args...", e.g. "ObjectTemplate.create Kit US_Assault".
//
// Language summary (all keywords and names are case-insensitive):
//   rem ...                      comment
//   beginRem / endRem            block comment (nests up to 10 deep)
//   beginNoExecution / endNoExecution
//                                check calls but don't execute them
//   var v_name [= value]         declare a variable in the current scope
//   const c_name = value         declare a constant
//   v_name = value               assign; "v_name" alone prints the value
//   if a [op b] / elseIf / else / endIf
//                                op: == != < <= > >= && || (or their word
//                                forms, e.g. greaterThan); "if ! a" / "if not a"
//   return                       stop executing the current file
//   run file [arg1..arg8]        run a script in a new scope (".con" default);
//                                it sees v_arg1..v_arg8, v_noArg and can set v_result
//   include file [args]          run a script in the caller's scope (".inc" default)
//   echo args...                 print the arguments (joined without spaces)
//   call ... -> v_name           store a call's output in a variable
//   call ... > file / >> file    write (append) a call's output to a file
//   quit / exit                  quit the game
// Arguments are split on whitespace; "double quotes" group words. v_ and c_
// arguments are replaced by their values.
//
// Reconstructed from BF2142.exe 1.51: line executor 0x00672310, script runner
// 0x00674430, run 0x006748d0, include 0x00671280, expressions 0x006960d0.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dice::hfe::io {
class FileManager;
}

namespace dice::hfe {

// Who may call a method. A line runs with a set of these; the method needs
// at least one in common. Developer methods also need developer mode.
namespace ConsoleAccess {
constexpr uint32_t User = 1;      // typed at the console
constexpr uint32_t Script = 2;    // script files and remote admin
constexpr uint32_t Developer = 4; // developer builds only
constexpr uint32_t Anyone = User | Script; // for methods
constexpr uint32_t All = User | Script | Developer; // for callers: scripts may call anything
} // namespace ConsoleAccess

enum class ConsoleMethodKind {
	Method,
	ReadWriteProperty, // no argument reads, one argument writes
	ReadOnlyProperty,
	WriteOnlyProperty,
};

struct ConsoleMethod {
	std::string name; // "object.method"
	ConsoleMethodKind kind = ConsoleMethodKind::Method;
	uint32_t access = ConsoleAccess::User | ConsoleAccess::Script;
	int minArgs = 0;
	int maxArgs = 0;
	// Methods that act on an "active" object (e.g. the template being
	// edited) are inactive while there is none, or when it isn't of their
	// class. Inactive methods are skipped by the lookup.
	std::function<bool()> isActive;
	std::function<std::string(const std::vector<std::string>& args)> invoke;
	// A stand-in for a method not rewritten yet (counted, does nothing): a
	// real one registered later with the same name replaces it.
	bool stub = false;
};

// The outcome of one line, as the original reports it.
enum class ConsoleResult {
	Ok,      // executed; output holds the result
	Skipped, // comment, keyword or nothing to print
	Error,   // output holds the message
	Echo,    // "echo"; output holds the text
};

class Console {
public:
	explicit Console(io::FileManager* files = nullptr);
	~Console();

	void registerMethod(ConsoleMethod method);
	const ConsoleMethod* findMethod(const std::string& object, const std::string& method) const;
	// Whether any method of that exact name is registered, active or not.
	bool hasMethod(const std::string& name) const;

	// Executes one line. `access` is a ConsoleAccess mask.
	ConsoleResult execute(const std::string& line, std::string& output, uint32_t access = ConsoleAccess::User);

	// Executes script text line by line; the outputs are appended to `output`.
	void executeScript(const std::string& text, std::string& output);

	// run / include. `output` receives v_result, or the error message on failure.
	bool run(const std::string& file, const std::vector<std::string>& args, std::string& output);
	bool include(const std::string& file, const std::vector<std::string>& args, std::string& output);

	// Variables (v_*) and constants (c_*), searched innermost scope first.
	bool getVariable(const std::string& name, std::string& value) const;
	bool getConstant(const std::string& name, std::string& value) const;
	// An engine constant (c_ETCar, ...), visible from every script.
	void registerConstant(const std::string& name, const std::string& value);

	// Where output goes: the in-game console window or a log.
	void setPrintHandler(std::function<void(const std::string&)> handler) { m_print = std::move(handler); }
	void setExitHandler(std::function<void()> handler) { m_exit = std::move(handler); }
	// Lines for which this returns true are ignored (used while loading).
	void setLineFilter(std::function<bool(const std::string&)> filter) { m_lineFilter = std::move(filter); }

	void setDeveloperMode(bool enabled) { m_developerMode = enabled; }
	// Script paths relative to the running script's directory (the default).
	void setRelativePaths(bool enabled) { m_relativePaths = enabled; }
	// Print errors, results and echo output from script files. The original
	// only prints echo output (when enabled); errors in scripts are silent.
	void setScriptOutput(bool echo, bool errors) { m_echoScripts = echo; m_printScriptErrors = errors; }

	const std::string& currentFile() const { return m_currentFile; }
	int currentLine() const { return m_currentLine; }

private:
	struct Scope;

	Scope& scope() { return *m_scopes.back(); }
	const Scope& scope() const { return *m_scopes.back(); }
	bool atTopLevel() const { return m_scopes.size() == 1; }
	bool pushScope();
	void popScope();

	bool declareVariable(const std::string& name, const std::string& value);
	bool declareConstant(const std::string& name, const std::string& value);
	bool setVariable(const std::string& name, const std::string& value);
	bool substitute(const std::string& word, std::string& value, std::string& error) const;

	int tokenize(const std::string& text, std::array<std::string, 13>& raw, std::array<std::string, 13>& args,
		int firstSubstituted, std::string& error) const;
	ConsoleResult callMethod(const std::string& object, const std::string& method, const std::vector<std::string>& args,
		uint32_t access, std::string& output);
	bool runFile(const std::string& file, const char* defaultExtension, bool newScope,
		const std::vector<std::string>& args, std::string& output);
	std::string resolveScriptPath(std::string file, const char* defaultExtension) const;
	void writeRedirectedOutput(const std::string& output);

	io::FileManager* m_files;
	std::multimap<std::string, ConsoleMethod> m_methods; // lower-case "object.method", registration order
	std::vector<std::unique_ptr<Scope>> m_scopes;   // [0] is the console itself

	std::function<void(const std::string&)> m_print;
	std::function<void()> m_exit;
	std::function<bool(const std::string&)> m_lineFilter;

	bool m_developerMode = false;
	bool m_relativePaths = true;
	bool m_echoScripts = false;
	bool m_printScriptErrors = false;

	std::string m_currentDirectory; // of the running script, with trailing '/'
	std::string m_currentFile;
	int m_currentLine = 0;
	std::string m_redirectFile; // "> file" of the line being executed
	bool m_redirectAppend = false;
};

// Evaluates "a", "! a", "not a" or "a op b" as the console's "if" does.
// Returns false with `error` set on a syntax error.
bool evaluateConsoleCondition(const std::string& lhs, const std::string& op, const std::string& rhs, bool& result,
	std::string& error);

} // namespace dice::hfe
