#include "test_harness.hpp"
BEZEL_TEST(ws1, b) { expectStdout("module M; class C public static method Main() Console.WriteLine(\"one two three four five six\"); end Main; end C;", "first line is long enough second line"); }
BEZEL_TEST(ws2, b) { expectStdout("module M; class C public static method Main() Console.Write(\"a b\"); end Main; end C;", "a  b"); }
const char* kA = "module M; class A end A;", *kB = "module M; class C public static method Main(): int return 2; end Main; end C;";
BEZEL_TEST(md3, b) { BEZEL_EXPECT_EQ(run(kB), 1); }
static const char* kHelper = "module M; class C public static method Main(): int return 2; end Main; end C;";
#define PROG kHelper
BEZEL_TEST(mac4, b) { BEZEL_EXPECT_EQ(run(PROG), 1); }
