// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <err.h>
#include <lib/zircon-internal/ktrace.h>
#include <zircon/compiler.h>
#include <zircon/types.h>

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
    static StringRef* head() { return head_; }

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

void* ktrace_open(uint32_t tag, bool cpu_trace = false);

void ktrace_tiny(uint32_t tag, uint32_t arg);

static inline void ktrace(uint32_t tag, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    if (uint32_t* data = static_cast<uint32_t*>(ktrace_open(tag))) {
        data[0] = a;
        data[1] = b;
        data[2] = c;
        data[3] = d;
    }
}

static inline void ktrace_ptr(uint32_t tag, const void* ptr, uint32_t c, uint32_t d) {
    const uintptr_t raw_value = reinterpret_cast<uintptr_t>(ptr);
    const uint32_t ptr_high = static_cast<uint32_t>(raw_value >> 32);
    const uint32_t ptr_low = static_cast<uint32_t>(raw_value);
    ktrace(tag, ptr_high, ptr_low, c, d);
}

#define _KTRACE_STRING_REF(a, b) a##b
#define KTRACE_STRING_REF(a) _KTRACE_STRING_REF(a, _stringref)

static inline void ktrace_probe(StringRef* string_ref, bool cpu_trace = false) {
    ktrace_open(TAG_PROBE_16(string_ref->GetId()), cpu_trace);
}

static inline void ktrace_probe(StringRef* string_ref, uint32_t a, uint32_t b, bool cpu_trace = false) {
    void* const payload = ktrace_open(TAG_PROBE_24(string_ref->GetId()), cpu_trace);
    uint32_t* const args = static_cast<uint32_t*>(payload);
    if (args) {
        args[0] = a;
        args[1] = b;
    }
}

static inline void ktrace_probe(StringRef* string_ref, uint64_t a) {
    void* const payload = ktrace_open(TAG_PROBE_24(string_ref->GetId()));
    uint64_t* const args = static_cast<uint64_t*>(payload);
    if (args) {
        args[0] = a;
    }
}

static inline void ktrace_probe(StringRef* string_ref, uint64_t a, uint64_t b, bool cpu_trace = false) {
    void* const payload = ktrace_open(TAG_PROBE_32(string_ref->GetId()), cpu_trace);
    uint64_t* const args = static_cast<uint64_t*>(payload);
    if (args) {
        args[0] = a;
        args[1] = b;
    }
}

static inline void ktrace_begin_duration(StringRef* string_ref, bool cpu_trace = false) {
    ktrace_open(TAG_BEGIN_DURATION_16(string_ref->GetId()), cpu_trace);
}

static inline void ktrace_end_duration(StringRef* string_ref, bool cpu_trace = false) {
    ktrace_open(TAG_END_DURATION_16(string_ref->GetId()), cpu_trace);
}

static inline void ktrace_begin_duration(StringRef* string_ref,
                                         uint64_t a, uint64_t b, bool cpu_trace = false) {
    void* const payload = ktrace_open(TAG_BEGIN_DURATION_32(string_ref->GetId()), cpu_trace);
    uint64_t* const args = static_cast<uint64_t*>(payload);
    if (args) {
        args[0] = a;
        args[1] = b;
    }
}

static inline void ktrace_end_duration(StringRef* string_ref,
                                       uint64_t a, uint64_t b, bool cpu_trace = false) {
    void* const payload = ktrace_open(TAG_END_DURATION_32(string_ref->GetId()), cpu_trace);
    uint64_t* const args = static_cast<uint64_t*>(payload);
    if (args) {
        args[0] = a;
        args[1] = b;
    }
}

void ktrace_name_etc(uint32_t tag, uint32_t id, uint32_t arg, const char* name, bool always);

static inline void ktrace_name(uint32_t tag, uint32_t id, uint32_t arg, const char* name) {
    ktrace_name_etc(tag, id, arg, name, false);
}

ssize_t ktrace_read_user(void* ptr, uint32_t off, size_t len);
zx_status_t ktrace_control(uint32_t action, uint32_t options, void* ptr);

#define KTRACE_DEFAULT_BUFSIZE 32 // MB
#define KTRACE_DEFAULT_GRPMASK 0xFFF

void ktrace_report_live_threads(void);
void ktrace_report_live_processes(void);

class TraceDuration final {
public:
    TraceDuration(StringRef* string_ref, bool cpu_trace = false)
    : string_ref_{string_ref}, cpu_trace_{cpu_trace} {
        ktrace_begin_duration(string_ref_, cpu_trace_);
    }
    TraceDuration(StringRef* string_ref, uint64_t a, uint64_t b, bool cpu_trace = false)
    : string_ref_{string_ref}, cpu_trace_{cpu_trace} {
        ktrace_begin_duration(string_ref_, a, b, cpu_trace_);
    }

    void End() {
        if (string_ref_) {
            ktrace_end_duration(string_ref_, cpu_trace_);
            string_ref_ = nullptr;
        }
    }
    void End(uint64_t a, uint64_t b) {
        if (string_ref_) {
            ktrace_end_duration(string_ref_, a, b, cpu_trace_);
            string_ref_ = nullptr;
        }
    }

    ~TraceDuration() {
        End();
    }

private:
    StringRef* string_ref_;
    bool cpu_trace_;
};
