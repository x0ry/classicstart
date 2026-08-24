// mini_test.h — a deliberately tiny test framework, in keeping with the
// project's own "no runtime dependencies beyond the OS" philosophy. No
// external test library, no package manager, just enough to register
// TEST cases across translation units and report CHECK failures.
#pragma once

#include <cstdio>
#include <vector>

struct MiniTestCase
{
    const char* name;
    void (*fn)();
};

inline std::vector<MiniTestCase>& MiniTestRegistry()
{
    static std::vector<MiniTestCase> tests;
    return tests;
}

struct MiniTestRegistrar
{
    MiniTestRegistrar(const char* name, void (*fn)())
    {
        MiniTestRegistry().push_back({ name, fn });
    }
};

inline int g_miniTestFailures = 0;
inline const char* g_miniTestCurrent = "";

#define TEST(name) \
    static void MiniTestFn_##name(); \
    static MiniTestRegistrar MiniTestReg_##name( \
        #name, \
        MiniTestFn_##name); \
    static void MiniTestFn_##name()

#define CHECK(cond) \
    do \
    { \
        if (!(cond)) \
        { \
            std::printf( \
                "  FAIL [%s] %s:%d: %s\n", \
                g_miniTestCurrent, \
                __FILE__, \
                __LINE__, \
                #cond); \
            ++g_miniTestFailures; \
        } \
    } while (0)

inline int RunAllMiniTests()
{
    int total = 0;

    for (auto& test : MiniTestRegistry())
    {
        ++total;

        g_miniTestCurrent = test.name;
        int before = g_miniTestFailures;

        test.fn();

        if (g_miniTestFailures == before)
            std::printf("  PASS %s\n", test.name);
    }

    std::printf(
        "\n%d test case(s), %d assertion failure(s)\n",
        total,
        g_miniTestFailures);

    return g_miniTestFailures == 0 ? 0 : 1;
}
