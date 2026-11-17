#include "test_harness.hpp"
// POSITIVE controls for --selftest: true duplicates of rows in ws_stdout_1001_test.cpp,
// written with a different C++ layout (literal splitting, comments, line breaks).
BEZEL_TEST(ws2, c) {
    expectStdout("module M; class C public static method Main() "  // split here
                 "Console.Write(\"a  b\"); end Main; end C;",
                 "a  b");
}
// same multi-declarator statement as 1001; the row reads the SECOND declarator
const char* kA = "module M; class A end A;",
           *kB = "module M; class C public static method Main(): int return 1; end Main; end C;";
BEZEL_TEST(md3, c) { BEZEL_EXPECT_EQ(run(kB), 1); }
// a row macro: its rows are counted by --count-rows, never grouped
#define ROW(NAME, SRC)                         \
    BEZEL_TEST(ws_rows, NAME) {                \
        BEZEL_EXPECT_EQ(run(SRC), 0);          \
    }
ROW(r1, "module M; class C public static method Main(): int return 0; end Main; end C;")
ROW(r2, "module M; class C public static method Main(): int return 0; end Main; end C;")
