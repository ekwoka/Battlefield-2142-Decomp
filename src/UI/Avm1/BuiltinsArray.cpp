// Array: the SWF 7 method set, including sort and sortOn with the
// CASEINSENSITIVE, DESCENDING, UNIQUESORT, RETURNINDEXEDARRAY and NUMERIC
// options.
#include "Builtins.h"

#include <algorithm>
#include <cmath>

namespace dice::hfe::ui::avm1::builtins {

namespace {

enum SortOption : int {
	kCaseInsensitive = 1,
	kDescending = 2,
	kUniqueSort = 4,
	kReturnIndexedArray = 8,
	kNumeric = 16,
};

ArrayObject* self(CallArgs& c) { return dynamic_cast<ArrayObject*>(c.thisObject.get()); }

// A start/end argument: negative counts from the end, clamped to [0, size].
size_t clampIndex(Interpreter& vm, const Value& v, size_t size, size_t otherwise)
{
	if (v.isUndefined()) {
		return otherwise;
	}
	double n = toInteger(vm.toNumber(v));
	if (n < 0) {
		n = std::max(0.0, static_cast<double>(size) + n);
	}
	return static_cast<size_t>(std::min(n, static_cast<double>(size)));
}

std::string join(Interpreter& vm, const std::vector<Value>& elements, const std::string& separator)
{
	std::string out;
	for (size_t i = 0; i < elements.size(); ++i) {
		if (i > 0) {
			out += separator;
		}
		out += vm.toString(elements[i]);
	}
	return out;
}

std::string lowerAscii(std::string s)
{
	for (char& c : s) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	return s;
}

// Stable bottom-up merge sort of indices. Unlike std::sort it stays in
// bounds when a script comparator is inconsistent.
template <typename Compare>
void mergeSort(std::vector<size_t>& order, Compare compare)
{
	std::vector<size_t> buffer(order.size());
	for (size_t width = 1; width < order.size(); width *= 2) {
		for (size_t lo = 0; lo < order.size(); lo += 2 * width) {
			const size_t mid = std::min(lo + width, order.size());
			const size_t hi = std::min(lo + 2 * width, order.size());
			size_t a = lo, b = mid, out = lo;
			while (a < mid && b < hi) {
				buffer[out++] = compare(order[a], order[b]) <= 0 ? order[a++] : order[b++];
			}
			while (a < mid) {
				buffer[out++] = order[a++];
			}
			while (b < hi) {
				buffer[out++] = order[b++];
			}
		}
		order.swap(buffer);
	}
}

// The default comparisons: as strings (optionally case-insensitive) or as
// numbers; DESCENDING flips them.
int compareValues(Interpreter& vm, const Value& a, const Value& b, int options)
{
	int result;
	if (options & kNumeric) {
		const double x = vm.toNumber(a);
		const double y = vm.toNumber(b);
		if (std::isnan(x) || std::isnan(y)) {
			result = std::isnan(x) == std::isnan(y) ? 0 : std::isnan(x) ? 1 : -1;
		} else {
			result = x < y ? -1 : x > y ? 1 : 0;
		}
	} else {
		std::string x = vm.toString(a);
		std::string y = vm.toString(b);
		if (options & kCaseInsensitive) {
			x = lowerAscii(std::move(x));
			y = lowerAscii(std::move(y));
		}
		result = x.compare(y) < 0 ? -1 : x == y ? 0 : 1;
	}
	return (options & kDescending) ? -result : result;
}

// Sorts `array` with `compare(a, b)` (an int) and applies the options:
// UNIQUESORT returns 0 when two elements compare equal, and
// RETURNINDEXEDARRAY returns the order instead of changing the array.
template <typename Compare>
Value sortWith(Interpreter& vm, ArrayObject& array, int options, Compare compare)
{
	const std::vector<Value> elements = array.elements; // the comparator may modify the array
	std::vector<size_t> order(elements.size());
	for (size_t i = 0; i < order.size(); ++i) {
		order[i] = i;
	}
	mergeSort(order, [&](size_t a, size_t b) { return compare(elements[a], elements[b]); });
	if (options & kUniqueSort) {
		for (size_t i = 1; i < order.size(); ++i) {
			if (compare(elements[order[i - 1]], elements[order[i]]) == 0) {
				return Value(0);
			}
		}
	}
	if (options & kReturnIndexedArray) {
		std::vector<Value> indices;
		for (size_t i : order) {
			indices.push_back(Value(i));
		}
		return Value(vm.newArray(std::move(indices)));
	}
	std::vector<Value> sorted;
	sorted.reserve(order.size());
	for (size_t i : order) {
		sorted.push_back(elements[i]);
	}
	array.elements = std::move(sorted);
	return Value(array.shared_from_this());
}

Value sort(Interpreter& vm, CallArgs& c)
{
	ArrayObject* array = self(c);
	if (!array) {
		return Value();
	}
	// sort(), sort(options), sort(compareFunction[, options])
	Value function;
	int options = 0;
	if (c[0].isFunction()) {
		function = c[0];
		options = c.size() > 1 ? toInt32(vm.toNumber(c[1])) : 0;
	} else if (c.size() > 0) {
		options = toInt32(vm.toNumber(c[0]));
	}
	if (function.isFunction()) {
		return sortWith(vm, *array, options, [&](const Value& a, const Value& b) {
			const double r = vm.toNumber(vm.call(function, nullptr, {a, b}));
			const int result = r < 0 ? -1 : r > 0 ? 1 : 0;
			return (options & kDescending) ? -result : result;
		});
	}
	return sortWith(vm, *array, options, [&](const Value& a, const Value& b) { return compareValues(vm, a, b, options); });
}

// sortOn("field" | ["f1", "f2"], options | [o1, o2])
Value sortOn(Interpreter& vm, CallArgs& c)
{
	ArrayObject* array = self(c);
	if (!array) {
		return Value();
	}
	std::vector<std::string> fields;
	std::vector<int> fieldOptions;
	if (const auto* names = dynamic_cast<ArrayObject*>(c[0].object().get())) {
		for (const Value& v : names->elements) {
			fields.push_back(vm.toString(v));
		}
	} else if (!c[0].isUndefined()) {
		fields.push_back(vm.toString(c[0]));
	}
	int options = 0;
	if (const auto* list = dynamic_cast<ArrayObject*>(c[1].object().get())) {
		for (const Value& v : list->elements) {
			fieldOptions.push_back(toInt32(vm.toNumber(v)));
		}
		// Per-field options only apply when they match the field count.
		if (fieldOptions.size() != fields.size()) {
			fieldOptions.clear();
		}
	} else if (c.size() > 1) {
		options = toInt32(vm.toNumber(c[1]));
	}
	if (fields.empty()) {
		return Value(array->shared_from_this());
	}
	const int flagsOfResult = fieldOptions.empty() ? options : fieldOptions[0];
	return sortWith(vm, *array, flagsOfResult, [&](const Value& a, const Value& b) {
		for (size_t i = 0; i < fields.size(); ++i) {
			const int o = fieldOptions.empty() ? options : fieldOptions[i];
			const int r = compareValues(vm, vm.getMember(a, fields[i]), vm.getMember(b, fields[i]), o);
			if (r != 0) {
				return r;
			}
		}
		return 0;
	});
}

} // namespace

void installArray(Interpreter& vm)
{
	const ObjectRef& proto = vm.prototypes().array;
	auto array = vm.newClass(
		[](Interpreter& vm, CallArgs& c) -> Value {
			std::shared_ptr<ArrayObject> out;
			if (c.constructing) {
				out = std::dynamic_pointer_cast<ArrayObject>(c.thisObject);
			}
			if (!out) {
				out = vm.newArray();
			}
			// Array(n) makes n undefined elements; anything else lists them.
			if (c.size() == 1 && c[0].isNumber()) {
				const double n = c[0].asNumber();
				if (n >= 0 && n == std::floor(n)) {
					out->setLength(n);
				} else {
					out->elements = c.args;
				}
			} else {
				out->elements = c.args;
			}
			return Value(out);
		},
		[](Interpreter& vm) -> ObjectRef { return vm.make<ArrayObject>(); }, proto);
	array->define("CASEINSENSITIVE", Value(kCaseInsensitive), kDontEnum | kDontDelete | kReadOnly);
	array->define("DESCENDING", Value(kDescending), kDontEnum | kDontDelete | kReadOnly);
	array->define("UNIQUESORT", Value(kUniqueSort), kDontEnum | kDontDelete | kReadOnly);
	array->define("RETURNINDEXEDARRAY", Value(kReturnIndexedArray), kDontEnum | kDontDelete | kReadOnly);
	array->define("NUMERIC", Value(kNumeric), kDontEnum | kDontDelete | kReadOnly);
	defineGlobal(vm, "Array", Value(array));

	vm.defineMethod(proto, "push", [](Interpreter&, CallArgs& c) -> Value {
		ArrayObject* a = self(c);
		if (!a) {
			return Value();
		}
		for (const Value& v : c.args) {
			if (a->elements.size() < ArrayObject::kMaxLength) {
				a->elements.push_back(v);
			}
		}
		return Value(a->elements.size());
	});
	vm.defineMethod(proto, "pop", [](Interpreter&, CallArgs& c) -> Value {
		ArrayObject* a = self(c);
		if (!a || a->elements.empty()) {
			return Value();
		}
		Value v = a->elements.back();
		a->elements.pop_back();
		return v;
	});
	vm.defineMethod(proto, "shift", [](Interpreter&, CallArgs& c) -> Value {
		ArrayObject* a = self(c);
		if (!a || a->elements.empty()) {
			return Value();
		}
		Value v = a->elements.front();
		a->elements.erase(a->elements.begin());
		return v;
	});
	vm.defineMethod(proto, "unshift", [](Interpreter&, CallArgs& c) -> Value {
		ArrayObject* a = self(c);
		if (!a) {
			return Value();
		}
		a->elements.insert(a->elements.begin(), c.args.begin(), c.args.end());
		a->setLength(static_cast<double>(a->elements.size()));
		return Value(a->elements.size());
	});
	vm.defineMethod(proto, "concat", [](Interpreter& vm, CallArgs& c) -> Value {
		ArrayObject* a = self(c);
		std::vector<Value> out = a ? a->elements : std::vector<Value> {};
		for (const Value& v : c.args) {
			if (const auto* other = dynamic_cast<ArrayObject*>(v.object().get())) {
				out.insert(out.end(), other->elements.begin(), other->elements.end());
			} else {
				out.push_back(v);
			}
		}
		if (out.size() > ArrayObject::kMaxLength) {
			out.resize(ArrayObject::kMaxLength);
		}
		return Value(vm.newArray(std::move(out)));
	});
	vm.defineMethod(proto, "join", [](Interpreter& vm, CallArgs& c) -> Value {
		ArrayObject* a = self(c);
		return a ? Value(join(vm, a->elements, c[0].isUndefined() ? std::string(",") : vm.toString(c[0]))) : Value();
	});
	vm.defineMethod(proto, "toString", [](Interpreter& vm, CallArgs& c) -> Value {
		ArrayObject* a = self(c);
		return a ? Value(join(vm, a->elements, ",")) : Value("[object Object]");
	});
	vm.defineMethod(proto, "reverse", [](Interpreter&, CallArgs& c) -> Value {
		ArrayObject* a = self(c);
		if (a) {
			std::reverse(a->elements.begin(), a->elements.end());
		}
		return Value(c.thisObject);
	});
	vm.defineMethod(proto, "slice", [](Interpreter& vm, CallArgs& c) -> Value {
		ArrayObject* a = self(c);
		if (!a) {
			return Value();
		}
		const size_t size = a->elements.size();
		const size_t from = clampIndex(vm, c[0], size, 0);
		const size_t to = clampIndex(vm, c[1], size, size);
		std::vector<Value> out;
		if (from < to) {
			out.assign(a->elements.begin() + static_cast<std::ptrdiff_t>(from), a->elements.begin() + static_cast<std::ptrdiff_t>(to));
		}
		return Value(vm.newArray(std::move(out)));
	});
	vm.defineMethod(proto, "splice", [](Interpreter& vm, CallArgs& c) -> Value {
		ArrayObject* a = self(c);
		if (!a || c.size() == 0) {
			return Value();
		}
		const size_t size = a->elements.size();
		const size_t from = clampIndex(vm, c[0], size, 0);
		size_t count = size - from;
		if (c.size() > 1) {
			const double n = toInteger(vm.toNumber(c[1]));
			count = static_cast<size_t>(std::max(0.0, std::min(n, static_cast<double>(size - from))));
		}
		const auto first = a->elements.begin() + static_cast<std::ptrdiff_t>(from);
		std::vector<Value> removed(first, first + static_cast<std::ptrdiff_t>(count));
		a->elements.erase(first, first + static_cast<std::ptrdiff_t>(count));
		if (c.size() > 2) {
			a->elements.insert(a->elements.begin() + static_cast<std::ptrdiff_t>(from), c.args.begin() + 2, c.args.end());
			a->setLength(static_cast<double>(a->elements.size()));
		}
		return Value(vm.newArray(std::move(removed)));
	});
	vm.defineMethod(proto, "sort", sort);
	vm.defineMethod(proto, "sortOn", sortOn);
}

} // namespace dice::hfe::ui::avm1::builtins
