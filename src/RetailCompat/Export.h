// Helpers for building drop-in replacements of the retail DLLs.
//
// The retail modules import each other's C++ functions by their MSVC-mangled
// names, but MinGW uses a different C++ ABI and GNU as can't even spell those
// symbols. Instead each replacement DLL defines plain extern "C" __cdecl
// forwarders and a .def file maps the original MSVC names onto them, e.g.
//   "?bf_free@memory@hfe@dice@@YAXPAX@Z" = Memory_bf_free
// This works for free functions with scalar/pointer arguments, which is all
// these interfaces use.
#pragma once

#if !defined(_WIN32) || !defined(__i386__)
#error "RetailCompat is only for 32-bit Windows builds"
#endif

#define RETAIL_EXPORT extern "C" __cdecl
