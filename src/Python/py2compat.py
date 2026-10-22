# Runs the game's Python 2 scripts (written for the Python 2.3 the retail
# game embeds in dice_py.dll) on Python 3.
#
# translate(source) rewrites Python 2 syntax to Python 3 with a small
# Python-2-aware tokenizer (print and exec statements, "except E, e",
# "raise E, v", backticks, <>, 0777 and 10L literals, ur'' strings), then
# rewrites Python 2 semantics in the syntax tree: "/" on two integers floors,
# dict.has_key / iteritems / keys() lists, list.sort(cmp) and sorted(x, cmp).
# Modules loaded from the game's directories get Python 2 builtins (map,
# filter, zip and range returning lists, xrange, reduce, cmp, long,
# unicode, basestring, apply...) and the string, types and __builtin__
# modules get their Python 2 contents.
import ast
import builtins
import functools
import importlib.abc
import importlib.machinery
import importlib.util
import os
import sys
import types

# --- tokens -----------------------------------------------------------------

_OPERATORS = sorted([
	'**=', '>>=', '<<=', '//=', '...', '!=', '<>', '==', '<=', '>=', '**', '//', '<<', '>>', '+=', '-=', '*=',
	'/=', '%=', '&=', '|=', '^=', '->', '+', '-', '*', '/', '%', '&', '|', '^', '~', '<', '>', '(', ')', '[',
	']', '{', '}', ',', ':', '.', ';', '@', '=', '`',
], key=len, reverse=True)


class Token:
	__slots__ = ('kind', 'text')

	def __init__(self, kind, text):
		self.kind = kind  # 'name', 'number', 'string', 'op', 'newline', 'space', 'comment', 'continuation'
		self.text = text

	def __repr__(self):
		return '%s(%r)' % (self.kind, self.text)


def tokenize(source):
	tokens = []
	i = 0
	n = len(source)
	while i < n:
		c = source[i]
		if c == '\n':
			tokens.append(Token('newline', '\n'))
			i += 1
		elif c in ' \t\r\f':
			j = i
			while j < n and source[j] in ' \t\r\f':
				j += 1
			tokens.append(Token('space', source[i:j]))
			i = j
		elif c == '\\' and i + 1 < n and source[i + 1] in '\r\n':
			j = i + 1
			if source[j] == '\r':
				j += 1
			if j < n and source[j] == '\n':
				j += 1
			tokens.append(Token('continuation', source[i:j]))
			i = j
		elif c == '#':
			j = source.find('\n', i)
			j = n if j < 0 else j
			tokens.append(Token('comment', source[i:j]))
			i = j
		elif c.isalpha() or c == '_':
			j = i
			while j < n and (source[j].isalnum() or source[j] == '_'):
				j += 1
			word = source[i:j]
			if j < n and source[j] in '\'"' and word.lower() in ('u', 'r', 'ur', 'b', 'br', 'rb', 'f', 'fr', 'rf'):
				end = _string_end(source, j)
				prefix = word.lower()
				if prefix == 'ur':
					word = 'r'  # Python 3 strings are unicode already
				tokens.append(Token('string', word + source[j:end]))
				i = end
			else:
				tokens.append(Token('name', word))
				i = j
		elif c.isdigit() or (c == '.' and i + 1 < n and source[i + 1].isdigit()):
			j = i
			while j < n and (source[j].isalnum() or source[j] in '._' or
							 (source[j] in '+-' and source[j - 1] in 'eE' and not source[i:j].lower().startswith('0x'))):
				j += 1
			tokens.append(Token('number', _fix_number(source[i:j])))
			i = j
		elif c in '\'"':
			end = _string_end(source, i)
			tokens.append(Token('string', source[i:end]))
			i = end
		else:
			for op in _OPERATORS:
				if source.startswith(op, i):
					tokens.append(Token('op', op))
					i += len(op)
					break
			else:
				tokens.append(Token('op', c))
				i += 1
	return tokens


def _string_end(source, i):
	quote = source[i]
	triple = source[i:i + 3] == quote * 3
	j = i + (3 if triple else 1)
	n = len(source)
	while j < n:
		if source[j] == '\\':
			j += 2
			continue
		if triple:
			if source[j:j + 3] == quote * 3:
				return j + 3
		elif source[j] == quote or source[j] == '\n':
			return j + 1
		j += 1
	return n


def _fix_number(text):
	if text[-1] in 'lL':
		text = text[:-1]
	if len(text) > 1 and text[0] == '0' and text.isdigit():
		text = '0o' + text[1:]
	return text


# --- statements -------------------------------------------------------------

def _significant(tokens, i):
	"""Index of the next token that is not space, comment or continuation."""
	while i < len(tokens) and tokens[i].kind in ('space', 'comment', 'continuation'):
		i += 1
	return i


def _statement_end(tokens, i):
	"""Index of the token ending the statement starting at i (newline or ; at depth 0)."""
	depth = 0
	while i < len(tokens):
		t = tokens[i]
		if t.kind == 'op':
			if t.text in '([{':
				depth += 1
			elif t.text in ')]}':
				depth -= 1
			elif t.text == ';' and depth == 0:
				return i
		elif t.kind == 'newline' and depth <= 0:
			return i
		i += 1
	return i


def _split_top(tokens):
	"""Splits tokens on commas at depth 0."""
	parts = [[]]
	depth = 0
	for t in tokens:
		if t.kind == 'op' and t.text in '([{':
			depth += 1
		elif t.kind == 'op' and t.text in ')]}':
			depth -= 1
		if t.kind == 'op' and t.text == ',' and depth == 0:
			parts.append([])
		else:
			parts[-1].append(t)
	return parts


def _text(tokens):
	return ''.join(t.text for t in tokens)


def _fix_print(tokens, start, end):
	"""print [>>f,] a, b[,]  ->  print(a, b[, end=' '][, file=f])"""
	body = tokens[start + 1:end]
	text = _text(body).strip()
	if text.startswith('(') and _matching(body) == len(body) - 1:
		return [Token('name', 'print'), Token('op', _text(body).lstrip())]  # already a call
	parts = _split_top(body)
	target = None
	if parts and _text(parts[0]).strip().startswith('>>'):
		target = _text(parts[0]).strip()[2:].strip()
		parts = parts[1:]
	trailing = bool(parts) and _text(parts[-1]).strip() == '' and len(parts) > 1
	if trailing:
		parts = parts[:-1]
	args = [_text(p).strip() for p in parts if _text(p).strip()]
	if trailing:
		args.append("end=' '")
	if target:
		args.append('file=' + target)
	return [Token('name', 'print(' + ', '.join(args) + ')')]


def _matching(body):
	"""Index in body of the parenthesis closing the first one, or -1."""
	depth = 0
	for i, t in enumerate(body):
		if t.kind == 'op' and t.text in '([{':
			depth += 1
		elif t.kind == 'op' and t.text in ')]}':
			depth -= 1
			if depth == 0:
				return i
	return -1


def translate(source):
	tokens = tokenize(source.replace('\r\n', '\n'))
	# Python 2 indents a tab to the next multiple of 8 columns.
	for k, t in enumerate(tokens):
		if t.kind == 'space' and (k == 0 or tokens[k - 1].kind in ('newline', 'continuation')):
			t.text = t.text.expandtabs(8)
	out = []
	i = 0
	at_start = True  # at the start of a statement
	depth = 0
	while i < len(tokens):
		t = tokens[i]
		if t.kind in ('space', 'comment', 'continuation'):
			out.append(t)
			i += 1
			continue
		if t.kind == 'newline':
			out.append(t)
			if depth <= 0:
				at_start = True
			i += 1
			continue
		if at_start and t.kind == 'name' and t.text in ('print', 'exec'):
			following = _significant(tokens, i + 1)
			nxt = tokens[following] if following < len(tokens) else None
			is_statement = nxt is None or not (nxt.kind == 'op' and nxt.text in ('=', '.', '+=', ',', ')'))
			if is_statement and t.text == 'print':
				end = _statement_end(tokens, i + 1)
				out.extend(_fix_print(tokens, i, end))
				i = end
				at_start = False
				continue
			if is_statement and t.text == 'exec' and not (nxt.kind == 'op' and nxt.text == '('):
				end = _statement_end(tokens, i + 1)
				body = _text(tokens[i + 1:end]).strip()
				code, _, scope = body.partition(' in ')
				out.append(Token('name', 'exec(%s%s)' % (code.strip(), ', ' + scope.strip() if scope else '')))
				i = end
				at_start = False
				continue
		if at_start and t.kind == 'name' and t.text == 'raise':
			end = _statement_end(tokens, i + 1)
			parts = _split_top(tokens[i + 1:end])
			if len(parts) >= 2:
				out.append(Token('name', 'raise %s(%s)' % (_text(parts[0]).strip(), _text(parts[1]).strip())))
				i = end
				at_start = False
				continue
		if t.kind == 'name' and t.text == 'except':
			# except E, e:  ->  except E as e:
			j = i + 1
			level = 0
			comma = -1
			while j < len(tokens) and not (tokens[j].kind == 'op' and tokens[j].text == ':' and level == 0):
				if tokens[j].kind == 'op' and tokens[j].text in '([{':
					level += 1
				elif tokens[j].kind == 'op' and tokens[j].text in ')]}':
					level -= 1
				elif tokens[j].kind == 'op' and tokens[j].text == ',' and level == 0:
					comma = j
				j += 1
			if comma >= 0:
				tokens[comma] = Token('name', ' as')
		if t.kind == 'op' and t.text == '<>':
			t = Token('op', '!=')
		if t.kind == 'op' and t.text == '`':
			# `expr`  ->  repr(expr)
			j = i + 1
			while j < len(tokens) and not (tokens[j].kind == 'op' and tokens[j].text == '`'):
				j += 1
			out.append(Token('name', 'repr(' + _text(tokens[i + 1:j]) + ')'))
			i = j + 1
			at_start = False
			continue
		if t.kind == 'op':
			if t.text in '([{':
				depth += 1
			elif t.text in ')]}':
				depth -= 1
			elif t.text in (';',) and depth == 0:
				out.append(t)
				i += 1
				at_start = True
				continue
			elif t.text == ':' and depth == 0:
				# a one-line suite starts a statement after the colon
				out.append(t)
				i += 1
				at_start = True
				continue
		out.append(t)
		at_start = False
		i += 1
	return _fix_tree(_text(out))


# --- semantics --------------------------------------------------------------

class _Py2Semantics(ast.NodeTransformer):
	def visit_BinOp(self, node):
		self.generic_visit(node)
		if isinstance(node.op, ast.Div):
			return ast.copy_location(ast.Call(ast.Name('__py2_div', ast.Load()), [node.left, node.right], []), node)
		return node

	def visit_AugAssign(self, node):
		self.generic_visit(node)
		if isinstance(node.op, ast.Div):
			load = _as_load(node.target)
			value = ast.Call(ast.Name('__py2_div', ast.Load()), [load, node.value], [])
			return ast.copy_location(ast.Assign([node.target], value), node)
		return node

	def visit_Call(self, node):
		self.generic_visit(node)
		f = node.func
		if isinstance(f, ast.Attribute):
			if f.attr == 'has_key' and len(node.args) == 1:
				return ast.copy_location(ast.Compare(node.args[0], [ast.In()], [f.value]), node)
			if f.attr in ('iteritems', 'iterkeys', 'itervalues') and not node.args:
				f.attr = f.attr[4:]
				return node
			if f.attr in ('keys', 'values', 'items') and not node.args and not node.keywords:
				return ast.copy_location(ast.Call(ast.Name('list', ast.Load()), [node], []), node)
			if f.attr == 'sort' and len(node.args) == 1:
				node.keywords.append(ast.keyword('key', _cmp_to_key(node.args[0])))
				node.args = []
				return node
		if isinstance(f, ast.Name) and f.id == 'sorted' and len(node.args) == 2:
			node.keywords.append(ast.keyword('key', _cmp_to_key(node.args[1])))
			node.args = node.args[:1]
		return node


def _cmp_to_key(func):
	return ast.Call(ast.Name('__py2_cmp_to_key', ast.Load()), [func], [])


def _as_load(target):
	import copy
	load = copy.deepcopy(target)
	for node in ast.walk(load):
		if hasattr(node, 'ctx'):
			node.ctx = ast.Load()
	return load


def _fix_tree(source):
	tree = ast.parse(source)
	tree = _Py2Semantics().visit(tree)
	ast.fix_missing_locations(tree)
	return tree


# --- builtins and modules ---------------------------------------------------

def _py2_div(a, b):
	if isinstance(a, int) and isinstance(b, int) and not isinstance(a, bool) or (type(a) is bool and type(b) is int):
		return a // b
	return a / b


def _cmp(a, b):
	return (a > b) - (a < b)


def _make_builtins():
	b = dict(builtins.__dict__)
	b.update({
		'__py2_div': _py2_div,
		'__py2_cmp_to_key': functools.cmp_to_key,
		'map': lambda f, *s: list(map(f, *s)) if f is not None else list(zip(*s)),
		'filter': lambda f, s: (type(s)() if isinstance(s, (str, tuple)) else []).__class__(filter(f, s))
			if isinstance(s, (str, tuple)) else list(filter(f, s)),
		'zip': lambda *s: list(zip(*s)),
		'range': lambda *a: list(range(*a)),
		'xrange': range,
		'reduce': functools.reduce,
		'cmp': _cmp,
		'long': int,
		'unicode': str,
		'basestring': str,
		'unichr': chr,
		'apply': lambda f, args=(), kwargs={}: f(*args, **kwargs),
		'raw_input': input,
		'intern': sys.intern,
		'file': open,
		'execfile': lambda path, g=None, l=None: exec(compile(translate(open(path).read()), path, 'exec'), g, l),
	})
	return b


def _py2_import(name, globals=None, locals=None, fromlist=(), level=-1):
	# Python 2 tries "import x" inside the importing module's package first.
	if level <= 0 and globals:
		package = globals.get('__package__') or (globals.get('__name__') if '__path__' in globals else None)
		if package:
			head = name.split('.')[0]
			try:
				spec = importlib.util.find_spec(package + '.' + head)
			except (ImportError, ValueError):
				spec = None
			if spec is not None:
				return builtins.__import__(name, globals, locals, fromlist, 1)
	return builtins.__import__(name, globals, locals, fromlist, 0)


PY2_BUILTINS = _make_builtins()
PY2_BUILTINS['__import__'] = _py2_import


def _install_modules():
	import string
	for name in ('split', 'join', 'lower', 'upper', 'strip', 'lstrip', 'rstrip', 'replace', 'find', 'rfind',
				 'count', 'capitalize', 'swapcase', 'zfill', 'ljust', 'rjust', 'center', 'expandtabs'):
		if not hasattr(string, name):
			setattr(string, name, getattr(str, name))
	string.join = lambda words, sep=' ': sep.join(words)
	string.split = lambda s, sep=None, maxsplit=-1: s.split(sep, maxsplit)
	string.atoi = lambda s, base=10: int(s, base)
	string.atof = float
	string.atol = int
	string.letters = string.ascii_letters
	string.lowercase = string.ascii_lowercase
	string.uppercase = string.ascii_uppercase
	for name, value in (('StringType', str), ('UnicodeType', str), ('IntType', int), ('LongType', int),
						('FloatType', float), ('ListType', list), ('TupleType', tuple), ('DictType', dict),
						('DictionaryType', dict), ('NoneType', type(None)), ('BooleanType', bool),
						('StringTypes', (str,)), ('ClassType', type), ('InstanceType', object),
						('FileType', object)):
		if not hasattr(types, name):
			setattr(types, name, value)
	sys.modules.setdefault('__builtin__', builtins)
	# Modules Python 3 no longer has.
	fpformat = types.ModuleType('fpformat')
	fpformat.fix = lambda x, digits: '%.*f' % (max(int(digits), 0), float(x))
	fpformat.sci = lambda x, digits: '%.*e' % (max(int(digits), 0), float(x))
	sys.modules.setdefault('fpformat', fpformat)
	import hashlib
	md5 = types.ModuleType('md5')
	md5.new = md5.md5 = lambda data=b'': hashlib.md5(data.encode('latin-1') if isinstance(data, str) else data)
	md5.digest_size = 16
	sys.modules.setdefault('md5', md5)
	if not hasattr(sys, 'maxint'):
		sys.maxint = sys.maxsize


# --- imports ----------------------------------------------------------------

class Py2Loader(importlib.machinery.SourceFileLoader):
	def source_to_code(self, data, path, *, _optimize=-1):
		source = data.decode('latin-1') if isinstance(data, bytes) else data
		return compile(translate(source), path, 'exec', dont_inherit=True)

	def exec_module(self, module):
		module.__dict__['__builtins__'] = PY2_BUILTINS
		super().exec_module(module)

	def get_code(self, fullname):
		# Always translate from source (no .pyc caching of translated code).
		path = self.get_filename(fullname)
		with open(path, 'rb') as f:
			return self.source_to_code(f.read(), path)


_game_roots = []


def _path_hook(path):
	full = os.path.abspath(path)
	if not any(full == root or full.startswith(root + os.sep) for root in _game_roots):
		raise ImportError('not a game directory')
	return importlib.machinery.FileFinder(path, (Py2Loader, ['.py']))


def install(roots):
	"""Python 2 imports for modules under `roots` (put first on sys.path)."""
	_install_modules()
	for root in roots:
		full = os.path.abspath(root)
		_game_roots.append(full)
		if root not in sys.path:
			sys.path.insert(0, root)
	if _path_hook not in sys.path_hooks:
		sys.path_hooks.insert(0, _path_hook)
	sys.path_importer_cache.clear()
