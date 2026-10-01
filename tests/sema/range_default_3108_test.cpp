// fix #3108 (§16.3/§16.4, v0.242): `default(Range<T>)` is a compile error.
//
// §16.4 promises "no `Range<T>` value with a zero step can exist". The
// `default` form all-zeroes `Range<T>` would yield step=0, falsifying the
// invariant; with `lo == hi == 0` and `IsEndInclusive = false` the iteration
// happens to be a no-op (so the bug is benign today), but the invariant
// statement is the load-bearing claim #3052's §16.4 work deleted the
// `GetEnumerator`-time zero-step guard on, and `default` is a second producer
// §16.4 does not enumerate.
//
// The fix joins §17.11's rejection set: `default(Range<T>)` is a compile error
// (and `default(Range<T>?)` stays rejected — §16.4 says the value's *form*
// cannot exist, independent of the nullability). The empty range is spelled
// `0..<0`, and §17.7's closed-constructor-surface rule bars `new Range<T>`.
// These drive the real Driver (the diagnostic is in the front-end typecheck).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

namespace {
namespace fs = std::filesystem;

std::string prog(const std::string& body) {
    return "module M; class C public static method Main(): int "
           "var r: Range<int>;" + body + " end Main; end C;";
}

int compileAndRun(const std::string& source) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_3108");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    int code = -1;
    if (r.success) {
        int status = std::system(exe.c_str());
        if (status != -1 && WIFEXITED(status)) code = WEXITSTATUS(status);
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return code;
}

bool compileFails(const std::string& source) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_3108_neg");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return !r.success;
}
}  // namespace

// §16.4 (the bug): under the pre-fix behavior, `default(Range<int>)` produces
// a zero-step `Range<int>` whose contained values are all zero. The well-formed
// `0..<0` outranks any non-default Range, so the compile failure (not the
// silent "loop never runs") is what we want.
BEZEL_TEST(range_default_3108, default_range_int_rejected_spec_16_4) {
    BEZEL_EXPECT_TRUE(compileFails(prog(
        "  r := default(Range<int>); "
        "  var n: int := 0; "
        "  for var i in r do n := n + 1; end; "
        "  return n + 40;")));
}

// §16.4 / §17.11: the same rejection via the bare `default(Range<int>)` form,
// not the `var r := default(...)` assignment. The diagnostic must fire at the
// `default` site, not the assignment.
BEZEL_TEST(range_default_3108, default_range_int_expression_rejected_spec_17_11) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C public static method Main(): int "
        "  var r := default(Range<int>); "
        "  return 0; "
        "end Main; end C;"));
}

// §16.4: Range<char> has the same invariant — the `default` form is rejected
// for the same reason (the `default` all-zeroes would give step=0). Pinned
// separately because `Range<char>` is the deserialization path §16.4 calls
// out by name.
BEZEL_TEST(range_default_3108, default_range_char_rejected_spec_16_4) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C public static method Main(): int "
        "  var r := default(Range<char>); "
        "  return 0; "
        "end Main; end C;"));
}

// §17.11 (no false positive): `default(Range<T>?)` is the NULL value of the
// nullable form (§17.11's "Reference types: `null` if the location is
// nullable" rule), not a zero-step Range — `null` is not a Range value, so
// §16.4's "no Range<T> value with a zero step can exist" is not violated. The
// fix targets the non-nullable form only.
BEZEL_TEST(range_default_3108, default_range_nullable_still_accepted_spec_17_11) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var r: Range<int>? := default(Range<int>?); "
        "  return 7; "
        "end Main; end C;"), 7);
}

// §16.4 (no false positive): the well-formed `0..<0` empty range is unaffected.
// It iterates zero times — exits 41 (the value + 40 lift), no fault, no warning.
BEZEL_TEST(range_default_3108, empty_range_0_dot_lt_0_unaffected_spec_16_4) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var r: Range<int> := 0..<0; "
        "  var n: int := 0; "
        "  for var i in r do n := n + 1; end; "
        "  return n + 40; "
        "end Main; end C;"), 40);
}

// §16.4 (no false positive): the inferred `var r := 1..5` is unaffected — it's
// a range EXPRESSION, the sole producer §16.4 enumerates. Sum 1+2+3+4+5 = 15,
// exits 15.
BEZEL_TEST(range_default_3108, inferred_range_expression_unaffected_spec_16_4) {
    BEZEL_EXPECT_EQ(compileAndRun(prog(
        "  r := 1..5; "
        "  var n: int := 0; "
        "  for var i in r do n := n + i; end; "
        "  return n;")), 15);
}

// §16.4 (no false positive): a Range stored via the type annotation `Range<int>`,
// initialized from a range expression (the path §16.5 considers). Sum 1..3 = 6.
BEZEL_TEST(range_default_3108, annotated_range_from_expression_unaffected_spec_16_5) {
    BEZEL_EXPECT_EQ(compileAndRun(prog(
        "  r := 1..3; "
        "  var n: int := 0; "
        "  for var i in r do n := n + i; end; "
        "  return n;")), 6);
}
