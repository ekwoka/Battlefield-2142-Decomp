// Date: milliseconds since 1970 UTC with the ECMA-262 calendar arithmetic,
// local time from the C library's time zone.
#include "Builtins.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>

namespace dice::hfe::ui::avm1::builtins {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kMsPerDay = 86400000.0;

double day(double t) { return std::floor(t / kMsPerDay); }

double daysFromCivil(double year, double month, double date)
{
	// Howard Hinnant's days_from_civil, in doubles; month 0-based and may
	// be out of range (it carries into the year).
	year += std::floor(month / 12);
	month = std::fmod(month, 12);
	if (month < 0) {
		month += 12;
	}
	const double y = month < 2 ? year - 1 : year;
	const double era = std::floor(y / 400);
	const double yoe = y - era * 400;
	const double m = month + 1;
	const double doy = std::floor((153 * (m > 2 ? m - 3 : m + 9) + 2) / 5);
	const double doe = yoe * 365 + std::floor(yoe / 4) - std::floor(yoe / 100) + doy;
	return era * 146097 + doe - 719468 + date - 1;
}

struct Fields {
	double year, month, date, hours, minutes, seconds, ms, weekday;
};

Fields split(double t)
{
	const double days = day(t);
	double z = days + 719468;
	const double era = std::floor(z / 146097);
	const double doe = z - era * 146097;
	const double yoe = std::floor((doe - std::floor(doe / 1460) + std::floor(doe / 36524) - std::floor(doe / 146096)) / 365);
	const double doy = doe - (365 * yoe + std::floor(yoe / 4) - std::floor(yoe / 100));
	const double mp = std::floor((5 * doy + 2) / 153);
	Fields f {};
	f.date = doy - std::floor((153 * mp + 2) / 5) + 1;
	f.month = mp < 10 ? mp + 2 : mp - 10;
	f.year = yoe + era * 400 + (f.month < 2 ? 1 : 0);
	double inDay = t - days * kMsPerDay;
	f.hours = std::floor(inDay / 3600000);
	inDay -= f.hours * 3600000;
	f.minutes = std::floor(inDay / 60000);
	inDay -= f.minutes * 60000;
	f.seconds = std::floor(inDay / 1000);
	f.ms = inDay - f.seconds * 1000;
	f.weekday = std::fmod(days + 4, 7);
	if (f.weekday < 0) {
		f.weekday += 7;
	}
	return f;
}

double compose(const Fields& f)
{
	const double time = ((f.hours * 60 + f.minutes) * 60 + f.seconds) * 1000 + f.ms;
	const double t = daysFromCivil(f.year, f.month, f.date) * kMsPerDay + time;
	return std::isfinite(t) && std::fabs(t) <= 8.64e15 ? t : kNaN;
}

// The local time zone's offset from UTC at `utc`, in milliseconds.
double localOffset(double utc)
{
	if (!std::isfinite(utc)) {
		return 0;
	}
	const auto seconds = static_cast<std::time_t>(std::floor(std::fmin(std::fmax(utc / 1000, 0.0), 2147483647.0)));
	const std::tm* local = std::localtime(&seconds);
	if (!local) {
		return 0;
	}
	const Fields asLocal {local->tm_year + 1900.0, double(local->tm_mon), double(local->tm_mday), double(local->tm_hour),
		double(local->tm_min), double(local->tm_sec), 0, 0};
	return compose(asLocal) - static_cast<double>(seconds) * 1000;
}

double toLocal(double utc) { return utc + localOffset(utc); }
double toUtc(double local) { return local - localOffset(local - localOffset(local)); }

double now()
{
	using namespace std::chrono;
	return static_cast<double>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

DateObject* self(CallArgs& c) { return dynamic_cast<DateObject*>(c.thisObject.get()); }

// "Wed Oct 22 14:03:00 GMT+0200 2026", the player's format.
std::string format(double utc)
{
	if (std::isnan(utc)) {
		return "Invalid Date";
	}
	static const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
	static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	const double offset = localOffset(utc);
	const Fields f = split(utc + offset);
	const int offsetMinutes = static_cast<int>(offset / 60000);
	const int absOffset = offsetMinutes < 0 ? -offsetMinutes : offsetMinutes;
	char buffer[80];
	std::snprintf(buffer, sizeof buffer, "%s %s %d %02d:%02d:%02d GMT%c%02d%02d %.0f", days[static_cast<int>(f.weekday)],
		months[static_cast<int>(f.month)], static_cast<int>(f.date), static_cast<int>(f.hours), static_cast<int>(f.minutes),
		static_cast<int>(f.seconds), offsetMinutes < 0 ? '-' : '+', absOffset / 60, absOffset % 60, f.year);
	return buffer;
}

// Year, month[, date, hours, minutes, seconds, ms] as Date() and Date.UTC
// take them; years 0-99 mean 1900-1999.
Fields fieldsFromArgs(Interpreter& vm, CallArgs& c)
{
	Fields f {1970, 0, 1, 0, 0, 0, 0, 0};
	double* slots[] = {&f.year, &f.month, &f.date, &f.hours, &f.minutes, &f.seconds, &f.ms};
	for (size_t i = 0; i < c.size() && i < 7; ++i) {
		*slots[i] = toInteger(vm.toNumber(c[i]));
		if (std::isnan(vm.toNumber(c[i]))) {
			*slots[0] = kNaN;
		}
	}
	if (f.year >= 0 && f.year <= 99) {
		f.year += 1900;
	}
	return f;
}

// The getters: one field of the local or UTC time.
void defineGetter(Interpreter& vm, const ObjectRef& proto, const std::string& name, double Fields::*field)
{
	const auto getter = [field](bool utc) {
		return [field, utc](Interpreter&, CallArgs& c) -> Value {
			const DateObject* d = self(c);
			if (!d || std::isnan(d->time)) {
				return Value(kNaN);
			}
			return Value(split(utc ? d->time : toLocal(d->time)).*field);
		};
	};
	vm.defineMethod(proto, "get" + name, getter(false));
	vm.defineMethod(proto, "getUTC" + name, getter(true));
}

// The setters: setHours(h[, m, s, ms]) replaces fields from `first` on,
// at most `count` of them.
void defineSetter(Interpreter& vm, const ObjectRef& proto, const std::string& name, int first, int count)
{
	const auto setter = [first, count](bool utc) {
		return [first, count, utc](Interpreter& vm, CallArgs& c) -> Value {
			DateObject* d = self(c);
			if (!d) {
				return Value();
			}
			const double base = std::isnan(d->time) ? 0 : d->time;
			Fields f = split(utc ? base : toLocal(base));
			double* slots[] = {&f.year, &f.month, &f.date, &f.hours, &f.minutes, &f.seconds, &f.ms};
			for (int i = 0; i < count && static_cast<size_t>(i) < c.size(); ++i) {
				*slots[first + i] = toInteger(vm.toNumber(c[static_cast<size_t>(i)]));
				if (std::isnan(vm.toNumber(c[static_cast<size_t>(i)]))) {
					*slots[0] = kNaN;
				}
			}
			const double t = compose(f);
			d->time = utc || std::isnan(t) ? t : toUtc(t);
			return Value(d->time);
		};
	};
	vm.defineMethod(proto, "set" + name, setter(false));
	vm.defineMethod(proto, "setUTC" + name, setter(true));
}

} // namespace

void installDate(Interpreter& vm)
{
	const ObjectRef& proto = vm.prototypes().date;
	auto date = vm.newClass(
		[](Interpreter& vm, CallArgs& c) -> Value {
			DateObject* d = c.constructing ? self(c) : nullptr;
			if (!d) {
				return Value(format(now())); // Date() as a function
			}
			if (c.size() == 0) {
				d->time = now();
			} else if (c.size() == 1) {
				d->time = toInteger(vm.toNumber(c[0]));
				if (std::isnan(vm.toNumber(c[0]))) {
					d->time = kNaN;
				}
			} else {
				const double t = compose(fieldsFromArgs(vm, c));
				d->time = std::isnan(t) ? t : toUtc(t);
			}
			return Value(c.thisObject);
		},
		[](Interpreter& vm) -> ObjectRef { return vm.make<DateObject>(); }, proto);
	defineGlobal(vm, "Date", Value(date));

	vm.defineMethod(date, "UTC", [](Interpreter& vm, CallArgs& c) -> Value { return Value(compose(fieldsFromArgs(vm, c))); });

	const auto time = [](Interpreter&, CallArgs& c) -> Value {
		const DateObject* d = self(c);
		return d ? Value(d->time) : Value(kNaN);
	};
	vm.defineMethod(proto, "getTime", time);
	vm.defineMethod(proto, "valueOf", time);
	vm.defineMethod(proto, "setTime", [](Interpreter& vm, CallArgs& c) -> Value {
		DateObject* d = self(c);
		if (!d) {
			return Value();
		}
		const double t = vm.toNumber(c[0]);
		d->time = std::isnan(t) ? kNaN : toInteger(t);
		return Value(d->time);
	});
	vm.defineMethod(proto, "toString", [](Interpreter&, CallArgs& c) -> Value {
		const DateObject* d = self(c);
		return Value(d ? format(d->time) : std::string("Invalid Date"));
	});
	vm.defineMethod(proto, "getTimezoneOffset", [](Interpreter&, CallArgs& c) -> Value {
		const DateObject* d = self(c);
		return d ? Value(-localOffset(d->time) / 60000) : Value(kNaN);
	});
	vm.defineMethod(proto, "getYear", [](Interpreter&, CallArgs& c) -> Value {
		const DateObject* d = self(c);
		return d && !std::isnan(d->time) ? Value(split(toLocal(d->time)).year - 1900) : Value(kNaN);
	});
	vm.defineMethod(proto, "setYear", [](Interpreter& vm, CallArgs& c) -> Value {
		DateObject* d = self(c);
		if (!d) {
			return Value();
		}
		Fields f = split(toLocal(std::isnan(d->time) ? 0 : d->time));
		f.year = toInteger(vm.toNumber(c[0]));
		if (f.year >= 0 && f.year <= 99) {
			f.year += 1900;
		}
		const double t = compose(f);
		d->time = std::isnan(t) ? t : toUtc(t);
		return Value(d->time);
	});

	defineGetter(vm, proto, "FullYear", &Fields::year);
	defineGetter(vm, proto, "Month", &Fields::month);
	defineGetter(vm, proto, "Date", &Fields::date);
	defineGetter(vm, proto, "Day", &Fields::weekday);
	defineGetter(vm, proto, "Hours", &Fields::hours);
	defineGetter(vm, proto, "Minutes", &Fields::minutes);
	defineGetter(vm, proto, "Seconds", &Fields::seconds);
	defineGetter(vm, proto, "Milliseconds", &Fields::ms);
	defineSetter(vm, proto, "FullYear", 0, 3);
	defineSetter(vm, proto, "Month", 1, 2);
	defineSetter(vm, proto, "Date", 2, 1);
	defineSetter(vm, proto, "Hours", 3, 4);
	defineSetter(vm, proto, "Minutes", 4, 3);
	defineSetter(vm, proto, "Seconds", 5, 2);
	defineSetter(vm, proto, "Milliseconds", 6, 1);
}

} // namespace dice::hfe::ui::avm1::builtins
