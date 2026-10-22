# Console scripts

Battlefield 2142 configures almost everything through console scripts: the
archives to mount, object templates, HUD layouts, levels, and settings.
`src/Console` reimplements the interpreter. This page describes the language
as the 1.51 executable runs it, including its quirks, because the shipped
content depends on them.

Sources: line executor `0x00672310`, script runner `0x00674430`, `run`
`0x006748d0`, `include` `0x00671280`, conditions `0x006960d0`.

## Files

| Extension | Loaded by | Scope |
|---|---|---|
| `.con` | `run file [args]` (default extension) | new scope |
| `.inc` | `include file [args]` (default extension) | caller's scope |
| `.tweak` | `include file.tweak` | caller's scope; a missing `.tweak` is not an error |

Paths are relative to the directory of the running script. A leading `/` or
`\` makes them relative to the mod root. Files are read through the virtual
file system, so they can come from archives.

The shipped content has about 14,000 script files with 1.8 million method
calls on 69 objects. `ObjectTemplate.*` and `Object.*` account for 88% of
the calls.

## Lines

Every line is either a keyword or `object.method arg1 arg2 ...`. Names are
case-insensitive. Arguments split on blanks, and `"double quotes"` group
words (the quotes are removed). A closing quote always ends the argument.
At most 13 arguments are read.

Arguments that start with `v_` or `c_` are replaced by the value of that
variable or constant. An unknown name is an error, which includes the
target of `-> v_name` (see below), so it has to be declared first.

| Keyword | Meaning |
|---|---|
| `rem ...` | comment |
| `beginRem` ... `endRem` | block comment, up to 10 deep, not at the console prompt |
| `beginNoExecution` ... `endNoExecution` | calls are checked but not executed |
| `var v_x [= value]` | declare a variable in the current scope; redeclaring in the same scope is an error |
| `const c_x = value` | declare a constant |
| `v_x = value` | assign an existing variable |
| `v_x` / `c_x` | print the value |
| `if a [op b]` | start a conditional block (opens a scope) |
| `elseIf a [op b]`, `else`, `endIf` | |
| `return` | skip the rest of the current file |
| `run file [arg1 .. arg8]` | run a `.con` file in a new scope |
| `include file [args]` | run a file in the caller's scope |
| `echo args` | print the arguments, joined **without** spaces |
| `quit`, `exit` | quit the game |
| `... -> v_x` | store the output of a call in `v_x` |
| `... > file`, `... >> file` | write or append the output to a file |

## Scripts with arguments

`run` declares these in the new scope:

- `v_arg1` .. `v_arg8`: the arguments, after substitution
- `v_noArg`: the position of the last non-empty argument
- `v_result`: empty; whatever the script assigns becomes the output of `run`

An `include` declares the same names, but declarations never overwrite, so
inside a `run` the included file sees its caller's `v_arg*` and shares its
`v_result`.

## Conditions

`if` and `elseIf` take one to three words:

- `a`: true if `a` is a non-zero integer (`atoi`)
- `! a`, `not a`: the negation
- `a op b` with `op` one of:

| Operator | Word form | Compares |
|---|---|---|
| `==`, `=` | `equals` | text |
| `!=` | `notEquals` | text |
| `<` `<=` `>` `>=` | `lessThan`, `lessOrEqualThan`, `greaterThan`, `greaterOrEqualThan` | numbers (`atof`) |
| `&&`, `&` | `and` | integers |
| `\|\|`, `\|` | `or` | integers |

An empty left-hand side can only be compared with `==`. That's how
`if v_arg1 == BF2Editor` works when no argument was passed.

## Quirks the rewrite keeps

- `>` preceded by a blank starts an output redirect anywhere in the line, so
  `if v_a > 1` really means `if v_a` with output sent to the file `1`.
  Scripts use `greaterThan` instead.
- Inside a branch that isn't executed, a nested `if` is counted, and the
  nested `else`/`elseIf` also counts as its end. A nested `if ... else ...
  endIf` in a skipped branch therefore ends the outer `if` early.
- `-> v_x` checks that at most two words follow the arrow, not one.
- `run` from the interactive console is ignored. It is allowed only in
  scripts and remote admin.
- Errors in script files are not shown. `Console::setScriptOutput` can show
  them for debugging.

## Methods

A method has a kind (method, read/write property, read-only property,
write-only property), access flags (user, script, developer), and a minimum
and maximum argument count. A property reads with no arguments and writes
with one. `object.getX` and `object.setX` also reach property `X`.

String arguments are passed through whole (a quoted argument keeps its
spaces). Other arguments are parsed with `istream >> value`, and results are
printed with `ostream << value`. Booleans are `0`/`1`, and vectors are written `x/y/z`.
`ConsoleBinding.h` registers ordinary C++ functions with these conversions.

`tools/decomp/console_methods.py` lists all methods registered in the
executable, with their signatures. It reads the static initializers that
`CreateStaticInitializers.java` recovers.

`bf2142 --api symbols/<module>.console.csv` registers every method of those
tables that the rewrite does not have yet as a stand-in that only counts its
calls (`ConsoleMethod::stub`). A real method registered later under the same
name replaces the stand-in, so methods the game binds when a round starts
(the commander's buttons, the spawn screen's kit choice) are found instead
of the stubs registered at start-up.
