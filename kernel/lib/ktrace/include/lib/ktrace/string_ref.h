// Copyright 2019 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <zircon/types.h>

// TODO(eieio): Replace with ktl equivalent.
#include <atomic>

// Represents an internalized string that may be referenced in traces by id to
// improve the efficiency of lables and other strings. This type does not define
// constructors or a destructor and contains trivially constructible members so
// that it may be aggregate-initialized to avoid static initializers and guards.
struct StringRef {
    static constexpr int kInvalidId = -1;

    const char* string{nullptr};
    std::atomic<int> id{kInvalidId};
    StringRef* next{nullptr};

    // Returns the numeric id for this string ref. If this is the first runtime
    // encounter with this string ref a new id is generated and the string ref
    // is added to the global linked list.
    int GetId() {
        const int ref_id = id.load(std::memory_order_relaxed);
        return ref_id == kInvalidId ? Register(this) : ref_id;
    }

    // Returns the head of the global string ref linked list.
    static StringRef* head() { return head_.load(std::memory_order_acquire); }

private:
    static int Register(StringRef* string_ref);

    static std::atomic<int> id_counter_;
    static std::atomic<StringRef*> head_;
};

// String literal template operator that generates a unique StringRef instance
// for the given string literal. This implementation uses the N3599 extension
// supported by Clang and GCC. C++20 ratified a slightly different syntax that
// is simple to switch to, once available, without affecting call sites.
//
// References:
//     http://open-std.org/JTC1/SC22/WG21/docs/papers/2013/n3599.html
//     http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2017/p0424r2.pdf
//
// Example:
//     ktrace_probe("probe_name"_stringref, ...);
//
template <typename T, T... chars>
inline StringRef* operator""_stringref() {
    static const char storage[] = {chars..., '\0'};
    static StringRef string_ref{storage};
    return &string_ref;
}

// Macro to convert a string literal expression to a string ref operator
// expression.
#define _KTRACE_STRING_REF(a, b) a##b
#define KTRACE_STRING_REF(a) _KTRACE_STRING_REF(a, _stringref)

