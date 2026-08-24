// Entry point for the ClassicShell test executable. Individual test files
// (test_*.cpp) register their TEST cases at static-init time; this just
// runs whatever is registered and reports pass/fail as the process exit
// code, so it plugs into CI or a pre-commit hook with no extra glue.
#include "mini_test.h"

int main()
{
    return RunAllMiniTests();
}
