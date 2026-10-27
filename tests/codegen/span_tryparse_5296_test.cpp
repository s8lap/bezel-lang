// Spec-derived regression tests for #5296 — the `Parse` / `TryParse` SPAN
// overloads of stdlib §9.1 / §9.2 / §9.3 (v0.313, #1810).
//
// Authority — stdlib §9.1 (the `span overloads (#1810)` paragraph):
//
//   "The span overloads (#1810) accept exactly the inputs the string forms
//    accept — same FormatException/OverflowException contract, no whitespace
//    tolerance — over a ReadOnlyStringSpan window without materializing a
//    substring; TryParse is shape (c) of the §1 Try convention."
//
//   public static method TryParse(s: ReadOnlyStringSpan, out result: int): bool;
//   (§9.2 declares the `double` / `float` span forms and §9.3 the `decimal` one,
//   each as `Parse(s: ReadOnlyStringSpan)` + its own `TryParse` overload.)
//
// Pre-fix, every one of these arms dispatched on the member name and arity
// alone and then ran the operand through `loadStringData` — a
// `CreateStructGEP(%bz.string, spanVal, 0)` on the span's `{ptr,i32,i32}`
// `%bz.arrayspan` value — or passed it straight to `bz_decimal_try_parse`. A
// `ReadOnlyStringSpan` is the §3.3 record `{Source, Start, Length}`, not a
// string pointer, so the module failed verifyModule and the compile died:
//
//   GEP base pointer is not a vector or a vector of pointers
//     %10 = getelementptr inbounds nuw %bz.string, %bz.arrayspan %9, i32 0, i32 0
//   Load operand must be a pointer.
//     %str.data = load ptr, %bz.arrayspan %10, align 8
//   bezelc: codegen error: IR verification failed (malformed IR emitted)
//
// and for `decimal`:
//   Call parameter type does not match function signature!
//    ptr  %dectp.r = call i32 @bz_decimal_try_parse(%bz.arrayspan %7, ptr %1)
//
// The fix materializes the window with `bz_str_substring` — the same lowering
// `ReadOnlyStringSpan.ToString()` (stdlib §3.3) uses — and parses that. The
// window's code points are the parse input, and §9.1's rule is that the answer
// is the answer the STRING form gives for the same text, so every row below
// pairs a window row with the equivalent string row.
//
// Row groups, per the FAIL-WITHOUT template ("N user rows fail pre-fix; M
// control rows pass in every leg"):
//   * USER  — the span forms the issue reproduces. Every one failed to COMPILE
//             pre-fix (the malformed-IR errors above), so each is also a
//             fail-without row.
//   * CTRL  — the string forms, and the non-parse span members, which must keep
//             passing in every leg; a fix that broke these is a regression.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

// A window over a literal: sp(start, len) of `lit`.
std::string spanOf(const std::string& lit, int start, int len) {
    return "var sp := new ReadOnlyStringSpan(\"" + lit + "\", " +
           std::to_string(start) + ", " + std::to_string(len) + "); ";
}

// `T.TryParse(sp, out var v)` printing ok then the value.
std::string tryParseSpan(const std::string& type, const std::string& lit,
                         int start, int len) {
    return spanOf(lit, start, len) +
           "var ok := " + type + ".TryParse(sp, out var v); "
           "Console.WriteLine(ok); Console.WriteLine(v); ";
}

std::string out(const std::string& body) {
    return runAotOut("module M; class C public static method Main(): int " +
                     body + " return 0; end Main; end C;");
}

}  // namespace

// ---- USER: the issue's exact repro -----------------------------------------
// int.TryParse(new ReadOnlyStringSpan("hello 42", 6, 2), out var v)
// §9.1: "42" is an input the string form accepts -> True / 42.
BEZEL_TEST(span_tryparse_5296, issue_repro_int_window_parses) {
    BEZEL_EXPECT_EQ(out(tryParseSpan("int", "hello 42", 6, 2)), "True\n42");
}

// The issue's second spelling: `"…".AsSpan(6, 2)` reaches the same arm.
BEZEL_TEST(span_tryparse_5296, issue_repro_via_aspan) {
    BEZEL_EXPECT_EQ(out(
        "var sp := \"hello 42 world\".AsSpan(6, 2); "
        "var ok := int.TryParse(sp, out var v); "
        "Console.WriteLine(ok); Console.WriteLine(v); "),
        "True\n42");
}

// The `out _` discard form the issue names: it reached the same malformed IR,
// so #5289's discard fix could not cover it.
BEZEL_TEST(span_tryparse_5296, issue_repro_out_discard) {
    BEZEL_EXPECT_EQ(out(
        "var sp := new ReadOnlyStringSpan(\"hello 42\", 6, 2); "
        "Console.WriteLine(int.TryParse(sp, out _)); "),
        "True");
}
BEZEL_TEST(span_tryparse_5296, issue_repro_out_discard_rejects) {
    BEZEL_EXPECT_EQ(out(
        "var sp := new ReadOnlyStringSpan(\"nope!!\", 0, 6); "
        "Console.WriteLine(int.TryParse(sp, out _)); "),
        "False");
}

// ---- USER: the window is the input, not the source -------------------------
// The load-bearing row for this fix. A window is a code-point range into a
// source, and the source CONTINUES past it: window (0,2) of "4212" is "42",
// not "4212". An implementation that handed `strtoll` the source's own buffer
// (or a non-length-bounded pointer into it) would read "4212" and answer
// False/4212. §9.1: the span form accepts "exactly the inputs the string forms
// accept", and the string form accepts "42".
BEZEL_TEST(span_tryparse_5296, window_is_bounded_at_its_own_end) {
    BEZEL_EXPECT_EQ(out(tryParseSpan("int", "4212", 0, 2)), "True\n42");
}
// The mirror: a window that stops short must NOT see the source's tail. Each
// prefix of "4212" is itself a valid int, so every one of these succeeds — with
// the value the WINDOW names. A parser handed the source's own (unbounded)
// buffer would answer 4212 for all three, and a parser that stopped at the
// source's NUL rather than the window's would reject the short ones.
BEZEL_TEST(span_tryparse_5296, window_does_not_overread_source_tail) {
    BEZEL_EXPECT_EQ(out(tryParseSpan("int", "4212", 0, 4)), "True\n4212");
    BEZEL_EXPECT_EQ(out(tryParseSpan("int", "4212", 0, 3)), "True\n421");
    BEZEL_EXPECT_EQ(out(tryParseSpan("int", "4212", 0, 1)), "True\n4");
    // And a window whose own text is malformed still rejects, even though the
    // source as a whole is a valid int ("x4212" -> window (1,4) = "4212").
    BEZEL_EXPECT_EQ(out(tryParseSpan("int", "x4212", 1, 4)), "True\n4212");
    BEZEL_EXPECT_EQ(out(tryParseSpan("int", "4212x", 0, 4)), "True\n4212");
    BEZEL_EXPECT_EQ(out(tryParseSpan("int", "42x", 0, 3)), "False\n0");
}

// A window whose text is valid but whose SOURCE would not be: proves the parse
// sees the window, not the whole string.
BEZEL_TEST(span_tryparse_5296, window_valid_while_source_is_not) {
    // "xx42" is not a valid int; its (2,2) window "42" is.
    BEZEL_EXPECT_EQ(out(tryParseSpan("int", "xx42", 2, 2)), "True\n42");
}

// ---- USER: the whole-string window (AsSpan(), the common shape) -------------
BEZEL_TEST(span_tryparse_5296, whole_string_window_parses) {
    BEZEL_EXPECT_EQ(out(
        "var sp := \"1234\".AsSpan(); "
        "var ok := int.TryParse(sp, out var v); "
        "Console.WriteLine(ok); Console.WriteLine(v); "),
        "True\n1234");
}

// ---- USER: §9.1's strict-fault contract, over a window --------------------
// Same FormatException/OverflowException contract as the string forms; no
// whitespace tolerance. TryParse is shape (c): false with out = default(T).
BEZEL_TEST(span_tryparse_5296, window_negative_parses) {
    BEZEL_EXPECT_EQ(out(tryParseSpan("int", "v=-17;", 2, 3)), "True\n-17");
}
BEZEL_TEST(span_tryparse_5296, window_rejects_trailing_garbage) {
    // "42x" is not an input (§9.1: "and nothing else") -> False, out = 0.
    BEZEL_EXPECT_EQ(out(tryParseSpan("int", "42x", 0, 3)), "False\n0");
}
BEZEL_TEST(span_tryparse_5296, window_rejects_empty) {
    BEZEL_EXPECT_EQ(out(tryParseSpan("int", "42", 1, 0)), "False\n0");
}
BEZEL_TEST(span_tryparse_5296, window_rejects_leading_whitespace) {
    // §9.3's strict-fault model: surrounding whitespace is not accepted.
    BEZEL_EXPECT_EQ(out(tryParseSpan("int", "a 42", 0, 3)), "False\n0");
}
BEZEL_TEST(span_tryparse_5296, window_rejects_overflow) {
    // #1134: out of range is `false`, not a silent wrap to the low 32 bits.
    BEZEL_EXPECT_EQ(out(tryParseSpan("int", "99999999999", 0, 11)), "False\n0");
}

// ---- USER: the other members stdlib declares the span form for -------------
BEZEL_TEST(span_tryparse_5296, long_window_parses) {
    BEZEL_EXPECT_EQ(out(tryParseSpan("long", "hi 77 x", 3, 2)), "True\n77");
}
BEZEL_TEST(span_tryparse_5296, double_window_parses) {
    BEZEL_EXPECT_EQ(out(tryParseSpan("double", "pi=3.5 tail", 3, 3)), "True\n3.5");
}
BEZEL_TEST(span_tryparse_5296, double_window_rejects) {
    BEZEL_EXPECT_EQ(out(tryParseSpan("double", "3.5.7", 0, 5)), "False\n0");
}
BEZEL_TEST(span_tryparse_5296, float_window_parses) {
    BEZEL_EXPECT_EQ(out(tryParseSpan("float", "f=-1.5;", 2, 4)), "True\n-1.5");
}
BEZEL_TEST(span_tryparse_5296, decimal_window_parses) {
    BEZEL_EXPECT_EQ(out(tryParseSpan("decimal", "d=12.25;", 2, 5)), "True\n12.25");
}
BEZEL_TEST(span_tryparse_5296, decimal_window_rejects) {
    BEZEL_EXPECT_EQ(out(tryParseSpan("decimal", "12.3.4", 0, 6)), "False\n0");
}

// §9.2: the float span form must be bounded like the double one — window (0,3)
// of "3.5.7" is "3.5", a valid input, and must not see the trailing ".7".
BEZEL_TEST(span_tryparse_5296, double_window_is_bounded) {
    BEZEL_EXPECT_EQ(out(tryParseSpan("double", "3.5.7", 0, 3)), "True\n3.5");
}

// ---- §4.6: a WIDTH-2/4 source ----------------------------------------------
// §4.6 gives a string an adaptive per-string storage width: 1, 2, or 4 bytes
// per code point, chosen by the LARGEST code point in it
// (Emitter::codePointsWidth, src/codegen/LlvmBackend_String.cpp:343). So
// "U+20AC42" (euro sign + "42") is a width-2 string, and its (1,2) window is
// still "42" — which IS an input the string form accepts, so §9.1 requires
// True/42. This row pins that a wide SOURCE does not make the window
// unparseable, which is the substantive claim about the fix.
BEZEL_TEST(span_tryparse_5296, ascii_window_of_a_wide_source_parses) {
    BEZEL_EXPECT_EQ(out(
        "var sp := new ReadOnlyStringSpan(\"€42\", 1, 2); "
        "var ok := int.TryParse(sp, out var v); "
        "Console.WriteLine(ok); Console.WriteLine(v); "),
        "True\n42");
}

// The window itself holds the non-ASCII code point. §9.1's grammar is ASCII
// ("an optional leading + or -, then one or more digits of that radix, and
// nothing else"), so this is not a valid input -> false, out = default. It must
// be a clean `false` and never a read of the 2-byte code point as if it were a
// byte (which is what a naive width-blind lowering would do).
BEZEL_TEST(span_tryparse_5296, non_ascii_window_is_not_an_input) {
    BEZEL_EXPECT_EQ(out(tryParseSpan("int", "€42", 0, 1)), "False\n0");
}
BEZEL_TEST(span_tryparse_5296, non_ascii_window_does_not_crash) {
    // Same shape through the discard form: the width-2 path must be a clean
    // `false`, not a malformed load.
    BEZEL_EXPECT_EQ(out(
        "var sp := new ReadOnlyStringSpan(\"€7\", 0, 2); "
        "Console.WriteLine(int.TryParse(sp, out _)); "),
        "False");
}

// A Latin-1 (width-1) code point is also outside the ASCII grammar, and must be
// rejected the same way the string form rejects it — NOT read as a byte.
BEZEL_TEST(span_tryparse_5296, latin1_window_is_not_an_input) {
    // "é" is U+00E9 = 0xE9 <= 0xFF, so this literal is width 1 (per
    // codePointsWidth). Its byte 0xE9 is not an ASCII digit.
    BEZEL_EXPECT_EQ(out(tryParseSpan("int", "é42", 0, 1)), "False\n0");
    // ... and the window that skips it is still perfectly valid.
    BEZEL_EXPECT_EQ(out(tryParseSpan("int", "é42", 1, 2)), "True\n42");
}

// ---- CTRL: the string forms, unchanged by this fix ------------------------
BEZEL_TEST(span_tryparse_5296, ctrl_int_tryparse_string_parses) {
    BEZEL_EXPECT_EQ(out(
        "var ok := int.TryParse(\"42\", out var v); "
        "Console.WriteLine(ok); Console.WriteLine(v); "),
        "True\n42");
}
BEZEL_TEST(span_tryparse_5296, ctrl_int_tryparse_string_rejects) {
    BEZEL_EXPECT_EQ(out(
        "var ok := int.TryParse(\"42x\", out var v); "
        "Console.WriteLine(ok); Console.WriteLine(v); "),
        "False\n0");
}
BEZEL_TEST(span_tryparse_5296, ctrl_long_tryparse_string_parses) {
    BEZEL_EXPECT_EQ(out(
        "var ok := long.TryParse(\"77\", out var v); "
        "Console.WriteLine(ok); Console.WriteLine(v); "),
        "True\n77");
}
BEZEL_TEST(span_tryparse_5296, ctrl_double_tryparse_string_parses) {
    BEZEL_EXPECT_EQ(out(
        "var ok := double.TryParse(\"3.5\", out var v); "
        "Console.WriteLine(ok); Console.WriteLine(v); "),
        "True\n3.5");
}
BEZEL_TEST(span_tryparse_5296, ctrl_float_tryparse_string_parses) {
    BEZEL_EXPECT_EQ(out(
        "var ok := float.TryParse(\"-1.5\", out var v); "
        "Console.WriteLine(ok); Console.WriteLine(v); "),
        "True\n-1.5");
}
BEZEL_TEST(span_tryparse_5296, ctrl_decimal_tryparse_string_parses) {
    BEZEL_EXPECT_EQ(out(
        "var ok := decimal.TryParse(\"12.25\", out var v); "
        "Console.WriteLine(ok); Console.WriteLine(v); "),
        "True\n12.25");
}

// The string form's own empty / whitespace / overflow rows must be unchanged —
// these are the same guards the span rows above now share.
BEZEL_TEST(span_tryparse_5296, ctrl_int_tryparse_string_rejects_edge_cases) {
    BEZEL_EXPECT_EQ(out(
        "var ok := int.TryParse(\"\", out var v); "
        "Console.WriteLine(ok); Console.WriteLine(v); "),
        "False\n0");
    BEZEL_EXPECT_EQ(out(
        "var ok := int.TryParse(\" 42\", out var v); "
        "Console.WriteLine(ok); Console.WriteLine(v); "),
        "False\n0");
    BEZEL_EXPECT_EQ(out(
        "var ok := int.TryParse(\"99999999999\", out var v); "
        "Console.WriteLine(ok); Console.WriteLine(v); "),
        "False\n0");
}

// ---- CTRL: the non-parse span members, which share the operand shape ------
BEZEL_TEST(span_tryparse_5296, ctrl_span_length_and_tostring_unaffected) {
    BEZEL_EXPECT_EQ(out(
        "var sp := new ReadOnlyStringSpan(\"hello 42\", 6, 2); "
        "Console.WriteLine(sp.Length); Console.WriteLine(sp.ToString()); "),
        "2\n42");
}
BEZEL_TEST(span_tryparse_5296, ctrl_span_slice_then_tryparse) {
    // Slice rebuilds the same record, so the fix must hold on the derived span.
    BEZEL_EXPECT_EQ(out(
        "var sp := new ReadOnlyStringSpan(\"ab 99 cd\", 0, 8).Slice(3, 2); "
        "var ok := int.TryParse(sp, out var v); "
        "Console.WriteLine(ok); Console.WriteLine(v); "),
        "True\n99");
}
// The materialized window must equal `ToString()` — the same lowering both use.
BEZEL_TEST(span_tryparse_5296, ctrl_window_matches_tostring) {
    BEZEL_EXPECT_EQ(out(
        "var sp := \"xx4212\".AsSpan(2, 4); "
        "Console.WriteLine(sp.ToString()); "
        "var ok := int.TryParse(sp, out var v); "
        "Console.WriteLine(ok); Console.WriteLine(v); "),
        "4212\nTrue\n4212");
}

// The fix materializes the window (a fresh +1 string) and releases it after the
// parse, so the interesting question is whether a REPEATED parse leaks. A
// loop of 200 window-parses is the shape that would expose a missing release:
// under the ASan oracle (BEZEL_AOT_FORCE_SANITIZE=address) a leak here is
// 200 unreleased +1 references and fails the row. Ordinary (non-ASan) runs
// still pin the VALUE: 200 x 1234 = 246800, which a stale or dropped `out`
// store would change.
BEZEL_TEST(span_tryparse_5296, repeated_window_parse_does_not_leak) {
    BEZEL_EXPECT_EQ(out(
        "var s := \"loop 1234 tail\"; "
        "var n := 0; "
        "for var i in 0..<200 do "
        "  var sp := s.AsSpan(5, 4); "
        "  var ok := int.TryParse(sp, out var v); "
        "  if ok then "
        "    n := n + v; "
        "  end; "
        "end; "
        "Console.WriteLine(n); "),
        "246800");
}

// ---- LEAK PROBES (integrator request 4) ------------------------------------
//
// The fix ALLOCATES a window slice (`bz_str_substring`, a fresh +1 string) on
// every span parse and must free it on EVERY exit path. `runAot` links the
// generated code with ASan but runs it with `detect_leaks=0`, so the rows
// above prove the VALUE and catch UAF / OOB, NOT leaks. These four rows use
// `runAotChecked`, whose `detectLeaks=true` turns on LeakSanitizer, so a
// 100-iteration loop is 100 unreleased +1 references if a path misses the
// `emitStringRelease`. Each row asserts the program's own value AND
// `r.ran` — runAotChecked surfaces a sanitizer report as ran=false with the
// report in `diagnostics`, so a leak fails the row rather than printing green.
//
// (a) success  — parse succeeds, slice created/used/released
// (b) failure  — parse rejects,   slice created/released
// (c) out-unassigned — `out _` discards the value; the release must still run
// (d) all five declared span forms (int/long/double/float/decimal), 100x each
namespace {

// 100 iterations of `<stmt>` (which must itself update `n`), then print it.
std::string loop100(const std::string& stmt) {
    return std::string("var n := 0; "
                       "for var i in 0..<100 do "
                       "  ") + stmt + " "
                       "end; "
                       "Console.WriteLine(n); ";
}

}  // namespace

// (a) SUCCESS path: 100 x int.TryParse over a valid window.
BEZEL_TEST(span_tryparse_5296, leak_success_path) {
    auto r = bezel::test::runAotChecked(
        std::string("module M; class C public static method Main(): int ") +
        loop100("var ok := int.TryParse(\"ab 42 cd\".AsSpan(3, 2), out var v); "
                "if ok then n := n + 1; end; ") +
        " return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// (b) FAILURE path: the window is not a valid int, so the parse rejects.
// The slice is still allocated before the rejection and must still be freed.
BEZEL_TEST(span_tryparse_5296, leak_failure_path) {
    auto r = bezel::test::runAotChecked(
        "module M; class C public static method Main(): int "
        "var n := 0; "
        "for var i in 0..<100 do "
        "  var ok := int.TryParse(\"xx abc yy\".AsSpan(3, 3), out var v); "
        "  if ok then "
        "    n := n + 1; "
        "  end; "
        "end; "
        "Console.WriteLine(n); "
        " return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// (c) `out` DISCARDED: `out _` throws the value away, so the only thing left
// holding the slice is the arm's own release.
BEZEL_TEST(span_tryparse_5296, leak_out_discarded_path) {
    auto r = bezel::test::runAotChecked(
        "module M; class C public static method Main(): int "
        "var n := 0; "
        "for var i in 0..<100 do "
        "  var ok := int.TryParse(\"ab 42 cd\".AsSpan(3, 2), out _); "
        "  if ok then "
        "    n := n + 1; "
        "  end; "
        "end; "
        "Console.WriteLine(n); "
        " return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// (d) Every one of the five span forms (stdlib §9.1 — `int`, and `long` by
// §9.1's `the others have analogous shapes` rule; §9.2 — `double` / `float`;
// §9.3 — `decimal`), 100 iterations apiece, in one program.
BEZEL_TEST(span_tryparse_5296, leak_all_five_span_forms) {
    auto r = bezel::test::runAotChecked(
        "module M; class C public static method Main(): int "
        "var n := 0; "
        "for var i in 0..<100 do "
        "  var a := int.TryParse(\"ab 42 cd\".AsSpan(3, 2), out var va); "
        "  var b := long.TryParse(\"ab 42 cd\".AsSpan(3, 2), out var vb); "
        "  var c := double.TryParse(\"ab 4.5 cd\".AsSpan(3, 3), out var vc); "
        "  var d := float.TryParse(\"ab 4.5 cd\".AsSpan(3, 3), out var vd); "
        "  var e := decimal.TryParse(\"ab 42 cd\".AsSpan(3, 2), out var ve); "
        "  if a and b and c and d and e then "
        "    n := n + 1; "
        "  end; "
        "end; "
        "Console.WriteLine(n); "
        " return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}
