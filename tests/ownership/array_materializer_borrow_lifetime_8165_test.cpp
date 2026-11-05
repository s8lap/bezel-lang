// Ownership — issue #8165: the escaping-closure check and the consume-site
// freeze must reach the owned local of a builtin `Array<T>` materializer
// (`var a := t.Split(",")`, `ToCharArray()`, `ToArray()`, the `?.` and `??`
// forms) exactly as they reach the class-instance twin
// (`var a := new Item(1)`).
//
// SPEC (v0.355).
//   stdlib §9.6: `public method Split(separator: string): owned
//     Array<string>;` / `public method ToCharArray(): owned Array<char>;` —
//     the production is an OWNED value the binding solely owns, disposed at
//     the end of its scope.
//   §28.3.8 *Borrow lifetime rules*, closure bullet: "A borrowed local may
//     not be captured by a closure that may outlive the borrow's source."
//     A closure capturing an owned scope-bound local, assigned to a local
//     declared in an enclosing block, outlives it.
//   §28.3.9 *The consume-site freeze*: "Consuming an owned binding — a local
//     or an `owned` parameter — while a live local borrow of its current
//     value exists is a compile error ... The consume sites are the table's
//     eleven rows — including ... reassignment of the binding itself".
//   §28.7 *Overwrite while borrowed*: "Reassigning a plain-`var` owned local
//     while a live local borrow of its current value exists is rejected by
//     the consume-site freeze (K5.2)".
//
// THE ORACLES. The two repros are the issue's own (probes g02 and p25 of
// the #8153 re-review). Before the fix g02 compiled and raised
// `IndexOutOfRangeException` at run time (the closure read the freed
// array) and p25 compiled with no diagnostic. Each rejection row names the
// specific diagnostic — the same text the class-instance twin draws, which
// the twin rows pin alongside. The controls (a closure that does not
// escape, an alias whose last use precedes the reassignment, a plain
// materializer local) must still compile and run clean under the strict
// oracle, so the widened checks do not over-reject.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

std::string prog(const std::string& body) {
    return "module M; import System; "
           "class Item "
           "  public Id: int; "
           "  public constructor(id: int) this.Id := id; end constructor; "
           "end Item; "
           "class C "
           "  public static method Main(): int "
           "    var t: string := \"a,bc,dc\"; "
           "    var s: string? := \"a,bc,dc\"; "
           "    var k := 0; "
           "    var i := 0; "
           "    while i < 100 do " +
           body +
           "      i := i + 1; "
           "    end while; "
           "    Console.WriteLine(k); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

constexpr const char* kOutlives = "which outlives it";
constexpr const char* kFrozen = "the borrow held by `b` is still live";

void expectRejected(const bezel::test::JitResult& r, const char* needle) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
}

void expectClean(const bezel::test::JitResult& r, const std::string& out) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// ===========================================================================
// (a) §28.3.8 closure bullet — the issue's g02: a closure capturing the
//     inner block's `.Split` local is assigned to `g`, declared outside it.
// ===========================================================================

BEZEL_TEST(array_materializer_borrow_lifetime_8165,
           spec_28_3_8_split_local_captured_by_escaping_closure_rejected) {
    expectRejected(runAot(prog(
                       "      var g: () -> int := lambda() => 0; "
                       "      if i > -1 then var a := t.Split(\",\"); "
                       "        g := lambda() => a[1].Length; end if; "
                       "      k := k + g(); ")),
                   kOutlives);
}

// The `.ToCharArray()` and `??` spellings of the same production.
BEZEL_TEST(array_materializer_borrow_lifetime_8165,
           spec_28_3_8_to_char_array_and_coalesce_locals_captured_by_escaping_closure_rejected) {
    expectRejected(runAot(prog(
                       "      var g: () -> int := lambda() => 0; "
                       "      if i > -1 then var a := t.ToCharArray(); "
                       "        g := lambda() => a.Count; end if; "
                       "      k := k + g(); ")),
                   kOutlives);
    expectRejected(runAot(prog(
                       "      var g: () -> int := lambda() => 0; "
                       "      if i > -1 then var a := s?.Split(\",\") ?? t.Split(\";\"); "
                       "        g := lambda() => a.Count; end if; "
                       "      k := k + g(); ")),
                   kOutlives);
}

// The class-instance twin (probe h01) — rejected before and after.
BEZEL_TEST(array_materializer_borrow_lifetime_8165,
           spec_28_3_8_class_twin_captured_by_escaping_closure_rejected) {
    expectRejected(runAot(prog(
                       "      var g: () -> int := lambda() => 0; "
                       "      if i > -1 then var a := new Item(1); "
                       "        g := lambda() => a.Id; end if; "
                       "      k := k + g(); ")),
                   kOutlives);
}

// CONTROL — the closure is invoked inside the block it captures from and
// never escapes it: admitted, and the array is freed once.
BEZEL_TEST(array_materializer_borrow_lifetime_8165,
           spec_28_3_8_split_local_captured_by_non_escaping_closure_admitted) {
    expectClean(runAotChecked(prog(
                    "      if i > -1 then var a := t.Split(\",\"); "
                    "        var g: () -> int := lambda() => a[1].Length; "
                    "        k := k + g(); end if; ")),
                "200\n");
}

// ===========================================================================
// (b) §28.3.9 / §28.7 consume-site freeze — the issue's p25: an alias taken,
//     the owner reassigned, the alias read.
// ===========================================================================

BEZEL_TEST(array_materializer_borrow_lifetime_8165,
           spec_28_7_split_local_reassigned_under_live_alias_rejected) {
    expectRejected(runAot(prog(
                       "      var a := t.Split(\",\"); var b := a; a := t.Split(\";\"); "
                       "      k := k + b.Count + a.Count; ")),
                   kFrozen);
}

// The class-instance twin (probe r52) — rejected before and after.
BEZEL_TEST(array_materializer_borrow_lifetime_8165,
           spec_28_7_class_twin_reassigned_under_live_alias_rejected) {
    expectRejected(runAot(prog(
                       "      var a := new Item(1); var b := a; a := new Item(2); "
                       "      k := k + b.Id + a.Id; ")),
                   kFrozen);
}

// CONTROL — the alias's last use precedes the reassignment, so the borrow
// is not live at the consume site (§28.3.9 "A borrow is live at a consume
// site when some path from the site reaches a later use of it"): admitted.
// Straight-line, outside a loop: inside the `while` body the class-instance
// twin is rejected too (the freeze's liveness is carried around the back
// edge), so a loop-bodied control would not separate the two spellings.
BEZEL_TEST(array_materializer_borrow_lifetime_8165,
           spec_28_3_9_split_local_reassigned_after_alias_last_use_admitted) {
    auto r = runAot(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var t: string := \"a,bc,dc\"; "
        "    var k := 0; "
        "    var a := t.Split(\",\"); var b := a; k := k + b.Count; "
        "    a := t.Split(\";\"); k := k + a.Count; "
        "    Console.WriteLine(k); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4\n");
}

// CONTROL — a plain materializer local, no borrow at all: clean under the
// strict oracle (probe s01).
BEZEL_TEST(array_materializer_borrow_lifetime_8165,
           spec_28_3_plain_split_local_stays_clean) {
    expectClean(runAotChecked(prog(
                    "      var a := t.Split(\",\"); k := k + a.Count; ")),
                "300\n");
}
