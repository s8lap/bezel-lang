// Regression tests for #7441 (the closure-over-a-holder and the span holder)
// -- a use of a local that holds a borrow of a disposed variable's value
// through a CLOSURE over a local borrow, or through a SPAN over the value,
// was accepted after a DOMINATING explicit disposal of the source:
//
//   var d := new D(7);
//   var b := d;                    // an alias: a holder
//   var g := lambda() => b.Ping(); // a closure that captures such a holder
//   d.Dispose();                   // the disposal DOMINATES the later use
//   return g();                    // invoking the closure -- compile error
//
//   var xs: Array<int> := [1, 2, 3];
//   var s := xs.AsSpan();          // a span over it: a holder
//   xs.Dispose();
//   return s[0];                   // a read through the holder -- compile error
//
// Language spec §28.2 (v0.357) "Use after a dominating explicit disposal":
//   "The same error reaches a use through a local borrow of the variable's
//    value: where a local holds a borrow of that value -- an alias (`var b :=
//    a;`, `const b := a;`), a `using var` pin (§28.10), a value that holds
//    the borrow (a span over it, a borrow-bearing record or choice, §9.7), a
//    closure that captures such a holder, or an iterator, enumerator or
//    lazy-query frame that carries it ... -- taken while the variable held
//    that value, and every value in the holder's source set (K2.2) has been
//    so disposed on every path to the use, a use of the holder -- a read or
//    member access through it, passing it, re-borrowing it, invoking the
//    closure, enumerating or disposing the frame -- is the same compile
//    error; the diagnostic names the use, the holder, the disposed source and
//    the disposing site."
//   "A holder whose source was disposed on only some paths to the use ... is
//    not rejected".
//
// Root cause: `bindHeldBorrows` -- the one place a local takes its §28.2
// source set -- knew an alias, a borrow-bearing record construction and a
// bound method reference. A lambda literal and a span-producing call took no
// source set, so `checkUseThroughDisposedBorrowHolder` had nothing to test.
//
// Every REJECT row was measured on the parent compiler COMPILING, and the
// program then reading freed memory (ASan heap-use-after-free; in checked
// disposal mode a DisposedAccessException through a closure, an
// IndexOutOfRangeException or a core dump through a span) -- the lane's
// holder matrix, see the commit body. `s.Length` alone ran clean there: a
// span's length is in the span, but "a read or member access through it" is
// the rule, whatever the member reads. The rows drive the real AOT path
// (`runAot`): a rejected program does not run.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// `Run` is called 200 times so an admitted program is also a leak and a
// use-after-free probe under the generated-code sanitizer leg.
std::string prog(const std::string& body) {
    return "module M; import System;\n"
           "public class D\n"
           "  private _n: int;\n"
           "  public constructor(n: int)\n"
           "    _n := n;\n"
           "  end constructor;\n"
           "  public method Ping(): int => _n;\n"
           "end D;\n"
           "public class C\n"
           "  public static method Show(d: D): int => d.Ping();\n"
           "  public static method Apply(f: () -> int): int => f();\n"
           "  public static method First(s: ArraySpan<int>): int => s[0];\n"
           "  public static method Run(kill: bool): int\n" +
           body +
           "  end Run;\n"
           "  public static method Main(): int\n"
           "    var total := 0;\n"
           "    var i := 0;\n"
           "    while i < 200 do\n"
           "      total := total + C.Run(i < 0);\n"
           "      i := i + 1;\n"
           "    end while;\n"
           "    Console.WriteLine(total);\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// What each holder kind is called.
constexpr const char* kClosure =
    "is a closure that captures a local borrow of that value (§27.5)";
constexpr const char* kClosurePlural =
    "is a closure that captures local borrows of those values (§27.5)";
constexpr const char* kSpan = "is a span over that value (§28.3)";

// The program is rejected, with the holder sentence's error naming the use
// (`use`: "use of `<holder>` after a dominating explicit Dispose() of
// <sources>") and the holder kind.
void expectHolderUseRejected(const bezel::test::JitResult& r, const char* use,
                             const char* kind) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, use));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kind));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "§28.2's use-after-dispose compile error"));
}

}  // namespace

// ===========================================================================
// (a) "a closure that captures such a holder" -- "invoking the closure".
// ===========================================================================

// The closure captures an alias AND takes a parameter; it is invoked after
// the alias's source is disposed.
BEZEL_TEST(ownership_closure_and_span_holder_use_after_dominating_dispose_7441,
           spec_28_2_closure_over_alias_invoked_after_dispose_rejected) {
    expectHolderUseRejected(
        runAot(prog("    var d := new D(7);\n"
                    "    var b := d;\n"
                    "    var g := lambda(x: int) => b.Ping() + x;\n"
                    "    d.Dispose();\n"
                    "    return g(1);\n")),
        "use of `g` after a dominating explicit Dispose() of `d`", kClosure);
}

// The capture is an ARGUMENT inside the closure body, not a receiver.
BEZEL_TEST(ownership_closure_and_span_holder_use_after_dominating_dispose_7441,
           spec_28_2_closure_passing_alias_invoked_after_dispose_rejected) {
    expectHolderUseRejected(
        runAot(prog("    var d := new D(7);\n"
                    "    var b := d;\n"
                    "    var g := lambda() => C.Show(b);\n"
                    "    d.Dispose();\n"
                    "    return g();\n")),
        "use of `g` after a dominating explicit Dispose() of `d`", kClosure);
}

// "passing it": a COPY of the closure is passed after the disposal. The copy
// carries the closure's source set.
BEZEL_TEST(ownership_closure_and_span_holder_use_after_dominating_dispose_7441,
           spec_28_2_copy_of_closure_over_alias_passed_after_dispose_rejected) {
    expectHolderUseRejected(
        runAot(prog("    var d := new D(7);\n"
                    "    var b := d;\n"
                    "    var g := lambda() => b.Ping();\n"
                    "    var k := g;\n"
                    "    d.Dispose();\n"
                    "    return C.Apply(k);\n")),
        "use of `k` after a dominating explicit Dispose() of `d`", kClosure);
}

// A closure over a closure over an alias: the outer closure captures a
// holder (the inner closure), so it is a holder of the same source.
BEZEL_TEST(ownership_closure_and_span_holder_use_after_dominating_dispose_7441,
           spec_28_2_closure_over_closure_over_alias_rejected) {
    expectHolderUseRejected(
        runAot(prog("    var d := new D(7);\n"
                    "    var b := d;\n"
                    "    var g := lambda() => b.Ping();\n"
                    "    var g2 := lambda() => g() + 1;\n"
                    "    d.Dispose();\n"
                    "    return g2();\n")),
        "use of `g2` after a dominating explicit Dispose() of `d`", kClosure);
}

// "every value in the holder's source set ... has been so disposed": two
// aliases of two sources, both disposed. The diagnostic names both sources.
BEZEL_TEST(ownership_closure_and_span_holder_use_after_dominating_dispose_7441,
           spec_28_2_closure_over_two_aliases_both_disposed_rejected) {
    expectHolderUseRejected(
        runAot(prog(
            "    var d := new D(7);\n"
            "    var e := new D(1);\n"
            "    var b := d;\n"
            "    var c := e;\n"
            "    var g := lambda() => (if kill then c.Ping() else b.Ping());\n"
            "    d.Dispose();\n"
            "    e.Dispose();\n"
            "    return g();\n")),
        "use of `g` after a dominating explicit Dispose() of `e` and `d`",
        kClosurePlural);
}

// A closure over a SPAN: the span is the holder the closure captures.
BEZEL_TEST(ownership_closure_and_span_holder_use_after_dominating_dispose_7441,
           spec_28_2_closure_over_span_invoked_after_dispose_rejected) {
    expectHolderUseRejected(
        runAot(prog("    var xs: Array<int> := [1, 2, 3];\n"
                    "    var s := xs.AsSpan();\n"
                    "    var g := lambda() => s[0];\n"
                    "    xs.Dispose();\n"
                    "    return g();\n")),
        "use of `g` after a dominating explicit Dispose() of `xs`", kClosure);
}

// ===========================================================================
// (b) "a value that holds the borrow (a span over it ...)".
// ===========================================================================

// "a read ... through it": an element read.
BEZEL_TEST(ownership_closure_and_span_holder_use_after_dominating_dispose_7441,
           spec_28_2_ranged_span_element_read_after_dispose_rejected) {
    expectHolderUseRejected(
        runAot(prog("    var xs: Array<int> := [1, 2, 3];\n"
                    "    var s := xs.AsSpan(1, 2);\n"
                    "    xs.Dispose();\n"
                    "    return s[0];\n")),
        "use of `s` after a dominating explicit Dispose() of `xs`", kSpan);
}

// "a ... member access through it".
BEZEL_TEST(ownership_closure_and_span_holder_use_after_dominating_dispose_7441,
           spec_28_2_span_member_access_after_dispose_rejected) {
    expectHolderUseRejected(
        runAot(prog("    var xs: Array<int> := [1, 2, 3];\n"
                    "    var s := xs.AsSpan();\n"
                    "    xs.Dispose();\n"
                    "    return s.Length;\n")),
        "use of `s` after a dominating explicit Dispose() of `xs`", kSpan);
}

// "passing it".
BEZEL_TEST(ownership_closure_and_span_holder_use_after_dominating_dispose_7441,
           spec_28_2_span_passed_after_dispose_rejected) {
    expectHolderUseRejected(
        runAot(prog("    var xs: Array<int> := [1, 2, 3];\n"
                    "    var s := xs.AsSpan();\n"
                    "    xs.Dispose();\n"
                    "    return C.First(s);\n")),
        "use of `s` after a dominating explicit Dispose() of `xs`", kSpan);
}

// A span of a span (`Slice`) holds the same source.
BEZEL_TEST(ownership_closure_and_span_holder_use_after_dominating_dispose_7441,
           spec_28_2_slice_of_span_read_after_dispose_rejected) {
    expectHolderUseRejected(
        runAot(prog("    var xs: Array<int> := [1, 2, 3];\n"
                    "    var s := xs.AsSpan();\n"
                    "    var t := s.Slice(1);\n"
                    "    xs.Dispose();\n"
                    "    return t[0];\n")),
        "use of `t` after a dominating explicit Dispose() of `xs`", kSpan);
}

// Enumerating the span.
BEZEL_TEST(ownership_closure_and_span_holder_use_after_dominating_dispose_7441,
           spec_28_2_span_enumerated_after_dispose_rejected) {
    expectHolderUseRejected(
        runAot(prog("    var xs: Array<int> := [1, 2, 3];\n"
                    "    var s := xs.AsSpan();\n"
                    "    xs.Dispose();\n"
                    "    var t := 0;\n"
                    "    for var x in s do\n"
                    "      t := t + x;\n"
                    "    end;\n"
                    "    return t;\n")),
        "use of `s` after a dominating explicit Dispose() of `xs`", kSpan);
}

// A STORE through the span writes freed memory.
BEZEL_TEST(ownership_closure_and_span_holder_use_after_dominating_dispose_7441,
           spec_28_2_span_element_store_after_dispose_rejected) {
    expectHolderUseRejected(
        runAot(prog("    var xs: Array<int> := [1, 2, 3];\n"
                    "    var s := xs.AsSpan();\n"
                    "    xs.Dispose();\n"
                    "    s[0] := 5;\n"
                    "    return 1;\n")),
        "use of `s` after a dominating explicit Dispose() of `xs`", kSpan);
}

// The span of an `OwningBuffer`.
BEZEL_TEST(ownership_closure_and_span_holder_use_after_dominating_dispose_7441,
           spec_28_2_owning_buffer_span_read_after_dispose_rejected) {
    expectHolderUseRejected(
        runAot(prog("    var buf := new OwningBuffer<int>(4);\n"
                    "    var s := buf.AsSpan();\n"
                    "    buf.Dispose();\n"
                    "    return s[0];\n")),
        "use of `s` after a dominating explicit Dispose() of `buf`", kSpan);
}

// ===========================================================================
// The two named controls (base-green; every other admitted twin is a corpus
// candidate in the lane's probe directory).
// ===========================================================================

// CONTROL 1 -- "every value in the holder's source set (K2.2) has been so
// disposed": the closure captures an alias of a disposed source AND a live
// owner, so its source set is not all disposed and the invocation is left to
// the runtime backstop (which this run never reaches: `kill` is false). Goes
// red under the plausible wrong fix that rejects a closure as soon as ONE of
// its captures is a holder of a disposed source.
BEZEL_TEST(ownership_closure_and_span_holder_use_after_dominating_dispose_7441,
           control_closure_over_dead_alias_and_live_owner_runs) {
    auto r = runAotChecked(prog(
        "    var d := new D(7);\n"
        "    var e := new D(1);\n"
        "    var b := d;\n"
        "    var g := lambda() => (if kill then b.Ping() else e.Ping());\n"
        "    d.Dispose();\n"
        "    return g();\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200\n"));
}

// CONTROL 2 -- "A holder whose source was disposed on only some paths to the
// use ... is not rejected": a span whose source has a CONDITIONAL kill. Goes
// red under the plausible wrong fix that marks a span holder dead at any
// explicit disposal of its source, dominating or not.
BEZEL_TEST(ownership_closure_and_span_holder_use_after_dominating_dispose_7441,
           control_span_with_conditional_kill_of_source_runs) {
    auto r = runAotChecked(prog("    var xs: Array<int> := [1, 2, 3];\n"
                                "    var s := xs.AsSpan();\n"
                                "    if kill then\n"
                                "      xs.Dispose();\n"
                                "    end;\n"
                                "    return s[0];\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200\n"));
}
