#include "Console.h"

#include "IO/File/FileManager.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace dice::hfe {

namespace {

constexpr size_t kMaxScopes = 10;   // the console's own scope plus 9 levels of run/if
constexpr int kMaxBlockDepth = 10;  // beginRem / beginNoExecution nesting
constexpr size_t kMaxArgs = 13;
constexpr size_t kMaxScriptArgs = 8; // v_arg1..v_arg8

std::string toLower(std::string s)
{
	for (char& c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

bool equalsIgnoringCase(const std::string& a, const char* b)
{
	size_t i = 0;
	for (; i < a.size() && b[i] != '\0'; ++i) {
		if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
			return false;
		}
	}
	return i == a.size() && b[i] == '\0';
}

bool startsWithIgnoringCase(const std::string& s, const char* prefix)
{
	for (size_t i = 0; prefix[i] != '\0'; ++i) {
		if (i >= s.size() ||
			std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) {
			return false;
		}
	}
	return true;
}

bool isBlank(char c) { return c == ' ' || c == '\t'; }

struct CaseInsensitiveLess {
	bool operator()(const std::string& a, const std::string& b) const { return toLower(a) < toLower(b); }
};

} // namespace

struct Console::Scope {
	std::map<std::string, std::string, CaseInsensitiveLess> variables;
	std::map<std::string, std::string, CaseInsensitiveLess> constants;
	int remDepth = 0;
	int noExecutionDepth = 0;
	int skippedIfDepth = 0; // "if"s nested inside a branch that isn't executed
	bool inIf = false;      // this scope belongs to an "if"
	bool executing = false; // the current branch runs
	bool branchTaken = false;
	bool seenElse = false;
	bool returned = false;
};

bool evaluateConsoleCondition(const std::string& lhs, const std::string& op, const std::string& rhs, bool& result,
	std::string& error)
{
	error.clear();
	result = false;
	const bool isEquals = op == "=" || op == "==" || op == "equals";
	if (lhs.empty()) {
		// An empty variable can only be compared for equality.
		if (isEquals) {
			result = lhs == rhs;
			return true;
		}
		error = "Error: Syntax error in expression!";
		return false;
	}
	const float left = static_cast<float>(std::atof(lhs.c_str()));
	const bool leftTrue = std::atoi(lhs.c_str()) != 0;
	if (op.empty()) {
		result = leftTrue;
		return true;
	}
	if ((lhs == "!" || lhs == "not") && rhs.empty()) {
		result = std::atoi(op.c_str()) == 0;
		return true;
	}
	if (rhs.empty()) {
		error = "Error: Syntax error in expression!";
		return false;
	}
	const float right = static_cast<float>(std::atof(rhs.c_str()));
	const bool rightTrue = std::atoi(rhs.c_str()) != 0;
	// Equality compares text; ordering compares numbers.
	if (isEquals) {
		result = lhs == rhs;
	} else if (op == "!=" || op == "notEquals") {
		result = lhs != rhs;
	} else if (op == ">" || op == "greaterThan") {
		result = left > right;
	} else if (op == ">=" || op == "greaterOrEqualThan") {
		result = left >= right;
	} else if (op == "<" || op == "lessThan") {
		result = left < right;
	} else if (op == "<=" || op == "lessOrEqualThan") {
		result = left <= right;
	} else if (op == "&" || op == "&&" || op == "and") {
		result = leftTrue && rightTrue;
	} else if (op == "|" || op == "||" || op == "or") {
		result = leftTrue || rightTrue;
	} else {
		error = "Error: Unknown operand in expression \"" + op + "\"!";
		return false;
	}
	return true;
}

Console::Console(io::FileManager* files) : m_files(files)
{
	m_scopes.push_back(std::make_unique<Scope>());
}

Console::~Console() = default;

void Console::registerMethod(ConsoleMethod method)
{
	std::string key = toLower(method.name);
	if (!method.stub) {
		const auto [first, last] = m_methods.equal_range(key);
		for (auto it = first; it != last;) {
			it = it->second.stub || method.replaces ? m_methods.erase(it) : std::next(it);
		}
	}
	m_methods.emplace(std::move(key), std::move(method));
}

bool Console::hasMethod(const std::string& name) const
{
	return m_methods.count(toLower(name)) != 0;
}

const ConsoleMethod* Console::findMethod(const std::string& object, const std::string& method) const
{
	const auto lookup = [&](const std::string& name) -> const ConsoleMethod* {
		// Several classes can register the same name (e.g. each template class
		// its own "ObjectTemplate.geometry"); the first one that is active,
		// i.e. applies to the current object, is used.
		const auto [first, last] = m_methods.equal_range(toLower(object + "." + name));
		for (auto it = first; it != last; ++it) {
			if (!it->second.isActive || it->second.isActive()) {
				return &it->second;
			}
		}
		return nullptr;
	};
	if (const ConsoleMethod* found = lookup(method)) {
		return found;
	}
	// "getX"/"setX" also reach property X. The original accepts the
	// write-only kind for "get" and the read-only kind for "set"; the call
	// then fails with "only setable"/"only readable", which we keep.
	if (startsWithIgnoringCase(method, "get")) {
		const ConsoleMethod* property = lookup(method.substr(3));
		if (property &&
			(property->kind == ConsoleMethodKind::ReadWriteProperty || property->kind == ConsoleMethodKind::WriteOnlyProperty)) {
			return property;
		}
	}
	if (startsWithIgnoringCase(method, "set")) {
		const ConsoleMethod* property = lookup(method.substr(3));
		if (property &&
			(property->kind == ConsoleMethodKind::ReadWriteProperty || property->kind == ConsoleMethodKind::ReadOnlyProperty)) {
			return property;
		}
	}
	return nullptr;
}

bool Console::pushScope()
{
	if (m_scopes.size() >= kMaxScopes) {
		return false;
	}
	m_scopes.push_back(std::make_unique<Scope>());
	return true;
}

void Console::popScope()
{
	if (m_scopes.size() > 1) {
		m_scopes.pop_back();
	}
}

bool Console::getVariable(const std::string& name, std::string& value) const
{
	for (auto it = m_scopes.rbegin(); it != m_scopes.rend(); ++it) {
		const auto found = (*it)->variables.find(name);
		if (found != (*it)->variables.end()) {
			value = found->second;
			return true;
		}
	}
	return false;
}

bool Console::getConstant(const std::string& name, std::string& value) const
{
	for (auto it = m_scopes.rbegin(); it != m_scopes.rend(); ++it) {
		const auto found = (*it)->constants.find(name);
		if (found != (*it)->constants.end()) {
			value = found->second;
			return true;
		}
	}
	return false;
}

bool Console::setVariable(const std::string& name, const std::string& value)
{
	for (auto it = m_scopes.rbegin(); it != m_scopes.rend(); ++it) {
		const auto found = (*it)->variables.find(name);
		if (found != (*it)->variables.end()) {
			found->second = value;
			return true;
		}
	}
	return false;
}

// Declarations never overwrite: an existing name in the current scope fails.
bool Console::declareVariable(const std::string& name, const std::string& value)
{
	return scope().variables.emplace(name, value).second;
}

void Console::registerConstant(const std::string& name, const std::string& value)
{
	m_scopes.front()->constants[name] = value;
}

bool Console::declareConstant(const std::string& name, const std::string& value)
{
	return scope().constants.emplace(name, value).second;
}

bool Console::substitute(const std::string& word, std::string& value, std::string& error) const
{
	if (startsWithIgnoringCase(word, "v_")) {
		if (!getVariable(word, value)) {
			error = "Unknown variable " + word + "!";
			return false;
		}
	} else if (startsWithIgnoringCase(word, "c_")) {
		if (!getConstant(word, value)) {
			error = "Unknown constant " + word + "!";
			return false;
		}
	} else {
		value = word;
	}
	return true;
}

// Splits the arguments after the command word. `raw` gets the words as
// written, `args` with variables and constants substituted (from index
// `firstSubstituted` on; earlier words are names and must not be quoted).
// Returns the number of arguments, or -1 with `error` set.
int Console::tokenize(const std::string& text, std::array<std::string, 13>& raw, std::array<std::string, 13>& args,
	int firstSubstituted, std::string& error) const
{
	int count = 0;
	size_t pos = 0;
	while (static_cast<size_t>(count) < kMaxArgs) {
		while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
			++pos;
		}
		if (pos >= text.size()) {
			break;
		}
		std::string word;
		bool quoted = false;
		bool closed = false;
		while (pos < text.size()) {
			const char c = text[pos++];
			if (!quoted && isBlank(c)) {
				break;
			}
			if (c == '"') {
				if (quoted) {
					closed = true; // a closing quote always ends the word
					break;
				}
				quoted = true;
				continue;
			}
			word.push_back(c);
		}
		if (quoted && !closed) {
			error = "no closing '\"'!";
			return -1;
		}
		if (!quoted && word.empty()) {
			break;
		}
		raw[count] = word;
		args[count] = word;
		if (count < firstSubstituted) {
			if (quoted) {
				error = "'\"' not allowed in arg " + std::to_string(count) + "!";
				return -1;
			}
		} else if (!substitute(word, args[count], error)) {
			return -1;
		}
		++count;
	}
	return count;
}

ConsoleResult Console::callMethod(const std::string& object, const std::string& name,
	const std::vector<std::string>& args, uint32_t access, std::string& output)
{
	const ConsoleMethod* method = findMethod(object, name);
	if (!method) {
		output = "Unknown object or method!";
		return ConsoleResult::Error;
	}
	if (method->isActive && !method->isActive()) {
		output = "Method is not active!";
		return ConsoleResult::Error;
	}
	const bool allowed = (method->access & ConsoleAccess::Developer)
		? m_developerMode && (access & ConsoleAccess::Script)
		: (method->access & access) != 0;
	if (!allowed) {
		output = "Unauthorised method!";
		return ConsoleResult::Error;
	}
	const int count = static_cast<int>(args.size());
	if (method->kind == ConsoleMethodKind::ReadOnlyProperty && count != 0) {
		output = "Property is only readable!";
		return ConsoleResult::Error;
	}
	if (method->kind == ConsoleMethodKind::WriteOnlyProperty && count == 0) {
		output = "Property is only setable!";
		return ConsoleResult::Error;
	}
	if ((method->kind == ConsoleMethodKind::ReadWriteProperty || method->kind == ConsoleMethodKind::WriteOnlyProperty) &&
		count >= 2) {
		output = "Properties only takes one argument!";
		return ConsoleResult::Error;
	}
	if (count < method->minArgs) {
		output = "Too few arguments, the min no of arguments is " + std::to_string(method->minArgs) + "!";
		return ConsoleResult::Error;
	}
	if (count > method->maxArgs) {
		output = "Too many arguments, the max no of arguments is " + std::to_string(method->maxArgs) + "!";
		return ConsoleResult::Error;
	}
	if (scope().noExecutionDepth > 0) {
		return ConsoleResult::Skipped; // checked, not executed
	}
	output = method->invoke ? method->invoke(args) : std::string();
	return ConsoleResult::Ok;
}

ConsoleResult Console::execute(const std::string& line, std::string& output, uint32_t access)
{
	output.clear();
	m_redirectFile.clear();
	if (m_lineFilter && m_lineFilter(line)) {
		return ConsoleResult::Skipped;
	}

	// "command > file" or ">> file": a '>' at the start or after whitespace.
	// (So "if v_a > 1" is a redirect too, in the original as well; scripts
	// use "greaterThan".)
	std::string text = line;
	const size_t redirect = line.find('>');
	if (redirect != std::string::npos &&
		(redirect == 0 || std::isspace(static_cast<unsigned char>(line[redirect - 1])))) {
		size_t pos = redirect + 1;
		m_redirectAppend = pos < line.size() && line[pos] == '>';
		if (m_redirectAppend) {
			++pos;
		}
		while (pos < line.size() && isBlank(line[pos])) {
			++pos;
		}
		size_t end = pos;
		while (end < line.size() && !isBlank(line[end])) {
			++end;
		}
		m_redirectFile = line.substr(pos, end - pos);
		text = line.substr(0, redirect);
	}

	size_t pos = 0;
	while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
		++pos;
	}
	const size_t wordEnd = std::find_if(text.begin() + static_cast<std::ptrdiff_t>(pos), text.end(),
		[](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }) - text.begin();
	const std::string word = text.substr(pos, wordEnd - pos);
	const std::string rest = text.substr(wordEnd);
	if (word.empty()) {
		return ConsoleResult::Skipped;
	}
	const auto is = [&](const char* keyword) { return equalsIgnoringCase(word, keyword); };
	const auto fail = [&](std::string message) {
		output = std::move(message);
		return ConsoleResult::Error;
	};

	if (is("rem") || scope().returned) {
		return ConsoleResult::Skipped;
	}

	// Block comments and no-execution blocks.
	if (is("beginRem")) {
		if (atTopLevel()) {
			return fail("\"beginRem\" not allowed on the highest level!");
		}
		if (scope().remDepth >= kMaxBlockDepth) {
			return fail("\"beginRem\" max depth has been reached!");
		}
		++scope().remDepth;
		return ConsoleResult::Skipped;
	}
	if (is("endRem")) {
		if (scope().remDepth < 1) {
			return fail("Can not have a \"endRem\" without a \"beginRem\"!");
		}
		--scope().remDepth;
		return ConsoleResult::Skipped;
	}
	if (scope().remDepth > 0) {
		return ConsoleResult::Skipped;
	}
	if (is("beginNoExecution")) {
		if (atTopLevel()) {
			return fail("\"beginNoExecution\" not allowed on the highest level!");
		}
		if (scope().noExecutionDepth >= kMaxBlockDepth) {
			return fail("\"beginNoExecution\" max depth has been reached!");
		}
		++scope().noExecutionDepth;
		return ConsoleResult::Skipped;
	}
	if (is("endNoExecution")) {
		if (scope().noExecutionDepth < 1) {
			return fail("Can not have a \"endNoExecution\" without a \"beginNoExecution\"!");
		}
		--scope().noExecutionDepth;
		return ConsoleResult::Skipped;
	}

	// Inside a branch that isn't taken only the if-keywords are looked at.
	if (scope().inIf && !scope().executing) {
		if (is("if")) {
			++scope().skippedIfDepth;
			return ConsoleResult::Skipped;
		}
		if (!is("elseIf") && !is("else") && !is("endIf")) {
			return ConsoleResult::Skipped;
		}
		if (scope().skippedIfDepth > 0) {
			// Original behaviour: "else"/"elseIf" of a skipped nested "if"
			// also count as its end, so a nested if/else/endIf inside a
			// skipped branch ends the outer "if" early. Kept for fidelity.
			--scope().skippedIfDepth;
			return ConsoleResult::Skipped;
		}
	}

	if (is("return")) {
		if (atTopLevel()) {
			return fail("\"return\" not allowed on the highest level!");
		}
		scope().returned = true;
		return ConsoleResult::Skipped;
	}
	if (is("quit") || is("exit")) {
		if (m_exit) {
			m_exit();
		}
		output = "*** Game will exit! ***";
		return ConsoleResult::Ok;
	}

	const bool isDeclaration = is("var") || is("const");
	std::array<std::string, kMaxArgs> raw;
	std::array<std::string, kMaxArgs> args;
	int count = tokenize(rest, raw, args, isDeclaration ? 1 : 0, output);
	if (count < 0) {
		return ConsoleResult::Error;
	}

	bool isEcho = false;
	bool isRun = false;
	bool isInclude = false;
	if (is("echo")) {
		isEcho = true;
	} else if (is("var")) {
		if (count != 1 && !(count == 3 && raw[1] == "=")) {
			return fail("\"var\" syntax: (variableName [= initValue])!");
		}
		if (!startsWithIgnoringCase(raw[0], "v_")) {
			return fail("The variable name must begin with \"v_\"!");
		}
		if (!declareVariable(raw[0], args[2])) {
			return fail("Variable redefinition \"" + raw[0] + "\"!");
		}
		return ConsoleResult::Skipped;
	} else if (is("const")) {
		if (count != 3 || raw[1] != "=") {
			return fail("\"const\" syntax: (constName = value)!");
		}
		if (!startsWithIgnoringCase(raw[0], "c_")) {
			return fail("The constant name must begin with \"c_\"!");
		}
		if (!declareConstant(raw[0], args[2])) {
			return fail("Constant redefinition \"" + raw[0] + "\"!");
		}
		return ConsoleResult::Skipped;
	} else if (is("if")) {
		if (atTopLevel()) {
			return fail("\"if\" not allowed on the highest level!");
		}
		if (count == 0) {
			return fail("\"if\" requires a expression!");
		}
		if (count > 3) {
			return fail("Syntax error in boolean expression!");
		}
		bool result = false;
		if (!evaluateConsoleCondition(args[0], args[1], args[2], result, output)) {
			return ConsoleResult::Error;
		}
		// Each "if" opens a scope; variables declared in it end at "endIf".
		if (!pushScope()) {
			return fail("\"if\" max depth has been reached!");
		}
		scope().inIf = true;
		scope().executing = result;
		scope().branchTaken = result;
		return ConsoleResult::Skipped;
	} else if (is("endIf")) {
		if (count > 0) {
			return fail("\"endIf\" takes no arguments!");
		}
		if (!scope().inIf) {
			return fail("Can not have an \"endIf\" without an \"if\"!");
		}
		popScope();
		return ConsoleResult::Skipped;
	} else if (is("else")) {
		if (count > 0) {
			return fail("\"else\" takes no arguments!");
		}
		if (!scope().inIf) {
			return fail("Can not have an \"else\" without an \"if\"!");
		}
		if (scope().seenElse) {
			return fail("Can not have an \"else\" after an \"else\"!");
		}
		scope().executing = !scope().branchTaken;
		scope().seenElse = true;
		return ConsoleResult::Skipped;
	} else if (is("elseIf")) {
		if (!scope().inIf) {
			return fail("Can not have an \"elseIf\" without an \"if\"!");
		}
		if (scope().seenElse) {
			return fail("Can not have an \"elseIf\" after an \"else\"!");
		}
		if (count == 0) {
			return fail("\"elseIf\" requires a expression!");
		}
		if (count > 3) {
			return fail("Syntax error in boolean expression!");
		}
		if (scope().branchTaken) {
			scope().executing = false;
		} else {
			bool result = false;
			if (!evaluateConsoleCondition(args[0], args[1], args[2], result, output)) {
				return ConsoleResult::Error;
			}
			scope().executing = result;
			scope().branchTaken = result;
		}
		return ConsoleResult::Skipped;
	} else if (startsWithIgnoringCase(word, "v_")) {
		// "v_x" prints the value, "v_x = value" assigns.
		std::string value;
		if (!substitute(word, value, output)) {
			return ConsoleResult::Error;
		}
		if (count == 0) {
			output = value;
			return ConsoleResult::Ok;
		}
		if (count != 2 || raw[0] != "=") {
			return fail("Wrong syntax!");
		}
		setVariable(word, args[1]);
		return ConsoleResult::Skipped;
	} else if (startsWithIgnoringCase(word, "c_")) {
		if (count != 0) {
			return fail("Wrong syntax!");
		}
		return substitute(word, output, output) ? ConsoleResult::Ok : ConsoleResult::Error;
	} else if (is("run")) {
		if (!(access & ConsoleAccess::Script)) {
			return ConsoleResult::Skipped; // silently ignored from the console
		}
		isRun = true;
	} else if (is("include")) {
		isInclude = true;
	}

	const size_t dot = word.find('.');
	if (dot == std::string::npos && !isRun && !isInclude && !isEcho) {
		return fail("Wrong syntax!");
	}

	// "... -> v_name" stores the output in a variable.
	std::string target;
	for (int i = 0; i < count; ++i) {
		if (raw[i] != "->") {
			continue;
		}
		if (count < i + 2) {
			return fail("A variable is required after \"->\"!");
		}
		if (count > i + 3) { // sic: one extra word is tolerated
			return fail("There can only be one variable after \"->\"!");
		}
		if (!startsWithIgnoringCase(raw[i + 1], "v_")) {
			return fail("The variable after \"->\" must have a name that begins with \"v_\"!");
		}
		target = raw[i + 1];
		count = i;
		break;
	}

	if (!isRun && !isInclude && !isEcho) {
		const std::vector<std::string> callArgs(args.begin(), args.begin() + count);
		const ConsoleResult result = callMethod(word.substr(0, dot), word.substr(dot + 1), callArgs, access, output);
		if (result != ConsoleResult::Ok) {
			return result;
		}
	}
	if (scope().noExecutionDepth > 0) {
		return ConsoleResult::Skipped;
	}

	if (isRun || isInclude) {
		if (count < 1) {
			return fail("\"run/include\" requires atleast 1 argument (filename [arg1 arg2 arg3 arg4])!");
		}
		if (static_cast<size_t>(count) > kMaxScriptArgs + 1) {
			return fail("\"run/include\" takes at most 8 arguments!");
		}
		const std::vector<std::string> scriptArgs(args.begin() + 1, args.begin() + count);
		const bool ok = isRun ? run(args[0], scriptArgs, output) : include(args[0], scriptArgs, output);
		if (!ok) {
			return ConsoleResult::Error;
		}
	} else if (isEcho) {
		output.clear();
		for (int i = 0; i < count; ++i) {
			output += args[i]; // sic: no separator
		}
		if (target.empty()) {
			return ConsoleResult::Echo;
		}
	}

	if (!target.empty() && !setVariable(target, output)) {
		declareVariable(target, output);
	}
	return ConsoleResult::Ok;
}

void Console::writeRedirectedOutput(const std::string& output)
{
	const std::string path = m_files ? m_files->expandPath(m_redirectFile) : m_redirectFile;
	std::ofstream file(path, m_redirectAppend ? std::ios::app : std::ios::trunc);
	if (!file) {
		if (m_print) {
			m_print("Failed to open file for writing: " + path);
		}
		return;
	}
	file << output;
}

void Console::executeScript(const std::string& text, std::string& output)
{
	std::istringstream lines(text);
	std::string line;
	while (std::getline(lines, line)) {
		++m_currentLine;
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		// Remote-admin commands in scripts are never executed.
		if (line.compare(0, 5, "rcon ") == 0) {
			output += "\n";
			continue;
		}
		if (line.empty()) {
			continue;
		}
		std::string result;
		switch (execute(line, result, ConsoleAccess::All)) {
		case ConsoleResult::Ok:
			if (!m_redirectFile.empty()) {
				writeRedirectedOutput(result);
			}
			break;
		case ConsoleResult::Error:
			if (m_printScriptErrors && m_print) {
				m_print("Error: " + result + " (" + m_currentFile + ":" + std::to_string(m_currentLine) + ": " + line + ")");
			}
			break;
		case ConsoleResult::Echo:
			if (m_echoScripts && m_print) {
				m_print("Echo: " + result);
			}
			break;
		case ConsoleResult::Skipped:
			break;
		}
		m_redirectFile.clear();
		output += result;
	}
}

std::string Console::resolveScriptPath(std::string file, const char* defaultExtension) const
{
	if (file.find('.') == std::string::npos) {
		file += defaultExtension;
	}
	if (m_relativePaths) {
		// A leading slash means relative to the mod root.
		if (!file.empty() && (file[0] == '/' || file[0] == '\\')) {
			file.erase(0, 1);
		} else {
			file = m_currentDirectory + file;
		}
	}
	std::replace(file.begin(), file.end(), '\\', '/');
	return file;
}

bool Console::run(const std::string& file, const std::vector<std::string>& args, std::string& output)
{
	return runFile(file, ".con", true, args, output);
}

bool Console::include(const std::string& file, const std::vector<std::string>& args, std::string& output)
{
	return runFile(file, ".inc", false, args, output);
}

bool Console::runFile(const std::string& file, const char* defaultExtension, bool newScope,
	const std::vector<std::string>& args, std::string& output)
{
	const std::string path = resolveScriptPath(file, defaultExtension);
	std::string text;
	if (!m_files || !m_files->readText(path, text)) {
		// Optional .tweak files may be missing.
		const size_t dot = path.rfind('.');
		if (!newScope && dot != std::string::npos && equalsIgnoringCase(path.substr(dot + 1), "tweak")) {
			return true;
		}
		output = "Could not open file: " + path + "!";
		return false;
	}
	if (text.empty()) {
		return true;
	}

	const size_t depth = m_scopes.size();
	if (newScope) {
		pushScope(); // at the depth limit the script shares the caller's scope
	}
	const std::string savedDirectory = m_currentDirectory;
	const std::string savedFile = m_currentFile;
	const int savedLine = m_currentLine;
	m_currentFile = path;
	m_currentLine = 0;
	if (m_relativePaths) {
		const size_t slash = path.rfind('/');
		m_currentDirectory = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
	}

	// Script arguments. An include shares its caller's scope, where these
	// already exist, so it sees the caller's arguments rather than its own.
	size_t argCount = 0;
	for (size_t i = 0; i < args.size() && i < kMaxScriptArgs; ++i) {
		if (!args[i].empty()) {
			argCount = i + 1;
		}
	}
	declareVariable("v_noArg", std::to_string(argCount));
	for (size_t i = 0; i < kMaxScriptArgs; ++i) {
		declareVariable("v_arg" + std::to_string(i + 1), i < args.size() ? args[i] : std::string());
	}
	declareVariable("v_result", "");

	std::string scriptOutput;
	executeScript(text + "\n", scriptOutput);
	output.clear();
	getVariable("v_result", output);

	m_currentDirectory = savedDirectory;
	m_currentFile = savedFile;
	m_currentLine = savedLine;
	while (m_scopes.size() > depth) {
		popScope();
	}
	return true;
}

} // namespace dice::hfe
