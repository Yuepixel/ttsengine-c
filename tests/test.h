// =============================================================================
//  test.h - micro test harness (no external dependency)
//
//  Every test file defines cases with TTSC_TEST(name) { ... } and asserts with
//  CHECK / CHECK_EQ / CHECK_NEAR. Registration happens at static init time, so
//  test_main.cpp only has to walk the registry.
// =============================================================================
#pragma once

#include <cmath>
#include <cstdio>
#include <exception>
#include <sstream>
#include <string>
#include <vector>

namespace ttsc_test {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

struct Reg {
    Reg(const char* name, void (*fn)()) { registry().push_back(Case{name, fn}); }
};

inline int& failures() {
    static int n = 0;
    return n;
}

inline void report(const char* file, int line, const std::string& msg) {
    ++failures();
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, msg.c_str());
}

}  // namespace ttsc_test

#define TTSC_TEST(name)                                              \
    static void name();                                              \
    static ::ttsc_test::Reg ttsc_reg_##name(#name, name);            \
    static void name()

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) ::ttsc_test::report(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        const auto va_ = (a);                                                    \
        const auto vb_ = (b);                                                    \
        if (!(va_ == vb_)) {                                                     \
            std::ostringstream os_;                                              \
            os_ << "CHECK_EQ(" #a ", " #b ") -> " << va_ << " vs " << vb_;       \
            ::ttsc_test::report(__FILE__, __LINE__, os_.str());                  \
        }                                                                        \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                    \
    do {                                                                         \
        const double va_ = double(a);                                            \
        const double vb_ = double(b);                                            \
        if (std::fabs(va_ - vb_) > double(tol)) {                                \
            std::ostringstream os_;                                              \
            os_ << "CHECK_NEAR(" #a ", " #b ") -> " << va_ << " vs " << vb_;     \
            ::ttsc_test::report(__FILE__, __LINE__, os_.str());                  \
        }                                                                        \
    } while (0)

// CHECK_THROWS(expr): the expression must throw std::exception or derived.
#define CHECK_THROWS(expr)                                                       \
    do {                                                                         \
        bool threw_ = false;                                                     \
        try {                                                                    \
            (void)(expr);                                                        \
        } catch (const std::exception&) {                                        \
            threw_ = true;                                                       \
        }                                                                        \
        if (!threw_) ::ttsc_test::report(__FILE__, __LINE__, "CHECK_THROWS(" #expr ")"); \
    } while (0)
