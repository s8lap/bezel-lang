#include "test_harness.hpp"
// #5233 scanner fixture (from review r42-rev-cull1-5233). NOT a test file: read only by
// scripts/test_dup_scan.py --selftest.  Each row here and in ws_stdout_1002_test.cpp differs from its
// namesake in the other file in a way the scanner must NOT fold away.
// case 1: expected stdout differs only in newline vs space (>= 40 chars)
BEZEL_TEST(ws1, a) { expectStdout("module M; class C public static method Main() Console.WriteLine(\"one two three four five six\"); end Main; end C;", "first line is long enough\nsecond line"); }
// case 2: a Bezel string literal with a double space (program >= 40 chars)
BEZEL_TEST(ws2, a) { expectStdout("module M; class C public static method Main() Console.Write(\"a  b\"); end Main; end C;", "a  b"); }
// case 3: second declarator
const char* kA = "module M; class A end A;", *kB = "module M; class C public static method Main(): int return 1; end Main; end C;";
BEZEL_TEST(md3, a) { BEZEL_EXPECT_EQ(run(kB), 1); }
// case 4: macro that references a helper defined differently in each file
static const char* kHelper = "module M; class C public static method Main(): int return 1; end Main; end C;";
#define PROG kHelper
BEZEL_TEST(mac4, a) { BEZEL_EXPECT_EQ(run(PROG), 1); }
