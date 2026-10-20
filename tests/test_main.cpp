// =============================================================================
//  test_main.cpp - runs every registered case
// =============================================================================
#include "test.h"

int main() {
    auto& cases = ttsc_test::registry();
    int   failed_cases = 0;

    for (const auto& c : cases) {
        const int before = ttsc_test::failures();
        c.fn();
        const int delta = ttsc_test::failures() - before;
        std::printf("%s  %s\n", delta == 0 ? "ok  " : "FAIL", c.name);
        if (delta != 0) ++failed_cases;
    }

    std::printf("\n%d case(s), %d failed, %d assertion failure(s)\n",
                int(cases.size()), failed_cases, ttsc_test::failures());
    return ttsc_test::failures() == 0 ? 0 : 1;
}
