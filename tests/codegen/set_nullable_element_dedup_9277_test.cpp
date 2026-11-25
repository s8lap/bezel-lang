// fix #9277 (stdlib §4.10 / §15.4 / §3.6 clause (c), v0.359): a `Set<T?>`
// did not deduplicate.
//
// THE DEFECT. A nullable element type is HASHABLE — §3.6's hashable row,
// clause (c): "a nullable type `T?` ... such a type's `Equals` and
// `GetHashCode` are `T`'s, through §8.4's null-safe wrapper — which hashes
// `null` to one fixed implementation-defined value" — so `Set<T?>` is a
// WELL-FORMED instantiation (stdlib §4.10: "`T` must be **hashable** (spec
// §3.6/§8.3), enforced at the instantiation site per §3.6 rule 2"), and
// §4.10's set semantics are owed to it: duplicates collapse, `TryAdd` returns
// "whether added (false if duplicate)", and `Remove` removes the element.
//
// The COMPILER put a nullable element on the ARRAY-backed set, whose dedup
// insert arms were gated on a CLASS element. The nullable element therefore
// fell through to the generic array APPEND: `Add` of an equal element grew
// `Count` (two copies of one element), `Remove` then removed only one copy
// (`Contains` stayed true), and `TryAdd` of a FRESH element had no handler at
// all — the call lowered to nothing and the `bool` result degraded to
// codegen's #2434 zero-init fallback, so it answered `false` and added
// nothing.
//
// THE IDENTITY a nullable element has. §8.4's null-safe wrapper over the
// UNDERLYING type's equality: `null` equals `null`, `null` differs from a
// present value, and two present values compare by the underlying type's own
// contract (content for a string, value for a primitive, the user's `Equals`
// for a class).
//
// RED ON THE BASE (`r30-c1` @ 009b0c5de), measured:
//   `Set<string?>`: `Add("x")` ×2 → `Count`        2   (required 1)
//   `Set<string?>`: `Add("x")` ×2, `Remove("x")`   Count 1 / `Contains` True
//                                                        (required 0 / False)
//   `Set<string?>`: `TryAdd("x")` ×2               False, False / `Count` 0
//                                                        (required True, False / 1)
//   `Set<string?>`: `Add(null)` ×2 → `Count`       2   (required 1)
//   `Set<int?>`:    `Add(1)` ×2 → `Count`           2   (required 1)
//   `Set<int?>`:    `TryAdd(null)`, `TryAdd(null)`, `TryAdd(7)`
//                                                        nothing printed
//                                                        (required True, False, True)
//
// The two BASE-GREEN controls at the end are the boundary rows a corpus case
// cannot express (a user-`Equals` equality through the null-safe wrapper; an
// `assertNoLeak` over 200 iterations of the §28.11 runtime-managed slot
// pairing) — each is red under a plausible wrong fix, named in the commit
// body.

#include <string>

#include "native_test_harness.hpp"
#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/run_under_asan.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

std::string stdoutOf(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.stdoutText : std::string("<did not run>");
}

// The §8.3 equality of a class element is its user `Equals` override (here:
// two instances with the same field are EQUAL), which the null-safe wrapper
// reaches through a nullable element type.
const char* kCell =
    "module M; import System; "
    "class Cell "
    "  public X: int; "
    "  public constructor(x: int) X := x; end constructor; "
    "  public override method Equals(other: Object?): bool "
    "    if other is Cell then var c := other as Cell; return c!.X = X; end; "
    "    return false; "
    "  end Equals; "
    "  public override method GetHashCode(): int => X; "
    "end Cell; "
    "class C ";

// Link+run `source` under ASan (LSan on). Returns 0 on a clean run matching
// `expectedExitCode`, 1 on any sanitizer report (leak, UAF, double-free) or
// a value mismatch, -1 if the toolchain is unavailable (SKIP).
int runUnderAsanValue(const std::string& source, int expectedExitCode,
                      const char* tag) {
    return bezel::test::runUnderAsanValue(source, expectedExitCode, tag,
                                          "detect_leaks=1:exitcode=99");
}

// Leak-checked. The `-1` (toolchain unavailable) case is NOT swallowed: the
// assertion runs on every other code, so the row cannot report a clean PASS
// with the sanitizer leg off.
void expectAsanClean(const std::string& source, int expectedExitCode) {
    const int a = runUnderAsanValue(source, expectedExitCode,
                                    "bezel_set_nullable_element_dedup_9277");
    if (a != -1) BEZEL_EXPECT_EQ(a, 0);
}

}  // namespace

// #9277 defect (1) — `Add` of the same element twice must collapse to one
// (stdlib §4.10 / §15.4: "Duplicate elements (per `T`'s equality contract)
// are silently collapsed"). Pre-fix: `Count` was 2.
BEZEL_TEST(set_nullable_element_dedup_9277,
           add_same_nullable_string_does_not_grow_count) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; import System; class C "
        "  public static method Main(): int "
        "    var s := new Set<string?>(); "
        "    s.Add(\"x\"); s.Add(\"x\"); "
        "    Console.WriteLine(s.Count); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "1\n");
}

// #9277 defect (2) — `Remove` must remove the element (§4.10: the element
// type supplies the identity). Pre-fix: one of the two copies went away, so
// `Count` was 1 and `Contains` was still `True`.
BEZEL_TEST(set_nullable_element_dedup_9277,
           remove_nullable_string_removes_the_element) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; import System; class C "
        "  public static method Main(): int "
        "    var s := new Set<string?>(); "
        "    s.Add(\"x\"); s.Add(\"x\"); "
        "    _ := s.Remove(\"x\"); "
        "    Console.WriteLine(s.Count); "
        "    Console.WriteLine(s.Contains(\"x\")); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "0\nFalse\n");
}

// #9277 defect (3) — `TryAdd` must report whether the element was added
// (stdlib §4.10: "returns whether added (false if duplicate)"). Pre-fix the
// call had no handler at all: the `bool` degraded to the #2434 zero-init
// fallback, so a FRESH element answered `false` and was not added.
BEZEL_TEST(set_nullable_element_dedup_9277,
           tryadd_nullable_string_reports_added_then_duplicate) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; import System; class C "
        "  public static method Main(): int "
        "    var s := new Set<string?>(); "
        "    var t1 := s.TryAdd(\"x\"); "
        "    var t2 := s.TryAdd(\"x\"); "
        "    Console.WriteLine($\"t1={t1} t2={t2} count={s.Count}\"); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "t1=True t2=False count=1\n");
}

// #9277 defect (4) — `null` is an element like any other: §3.6 clause (c)
// hashes `null` to ONE fixed value, so two `null`s are ONE element
// (§8.4's null-safe wrapper: `null = null` is `true`). Pre-fix: `Count` was 2.
BEZEL_TEST(set_nullable_element_dedup_9277,
           add_null_twice_collapses_to_one_element) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; import System; class C "
        "  public static method Main(): int "
        "    var s := new Set<string?>(); "
        "    s.Add(null); s.Add(null); "
        "    Console.WriteLine(s.Count); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "1\n");
}

// #9277 defect (5) — the same collapse for a NULLABLE VALUE element (the
// §17.5 tagged `{i8 hasValue, T value}` carrier), whose identity is the
// underlying `int`'s value equality. Pre-fix: `Count` was 2.
BEZEL_TEST(set_nullable_element_dedup_9277,
           add_same_nullable_int_does_not_grow_count) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; import System; class C "
        "  public static method Main(): int "
        "    var s := new Set<int?>(); "
        "    s.Add(1); s.Add(1); "
        "    Console.WriteLine(s.Count); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "1\n");
}

// #9277 defect (6) — the §3.6 clause (c) shape, verbatim: `null` hashes to
// one fixed value, so the first `TryAdd(null)` adds it and the second reports
// the duplicate, while a DIFFERENT element is still added. Pre-fix the whole
// statement emitted nothing (the `TryAdd` result did not lower), so the
// program printed no line at all.
BEZEL_TEST(set_nullable_element_dedup_9277,
           tryadd_nullable_int_null_then_value) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "module M; import System; class C "
        "  public static method Main(): int "
        "    var seen := new Set<int?>(); "
        "    Console.WriteLine(seen.TryAdd(null)); "
        "    Console.WriteLine(seen.TryAdd(null)); "
        "    Console.WriteLine(seen.TryAdd(7)); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "True\nFalse\nTrue\n");
}

// BASE-GREEN CONTROL (1 of 2). A NULLABLE CLASS element — the twin the issue
// records as already correct — must keep §8.3's EQUALITY contract: two
// DISTINCT instances that are equal per the user's `Equals` override are ONE
// element (the null-safe wrapper reaches the override when both operands are
// present). Green on the base; RED under a wrong fix that compared a nullable
// element by reference identity instead of the user's `Equals`.
BEZEL_TEST(set_nullable_element_dedup_9277,
           nullable_class_element_honours_user_equals) {
    BEZEL_EXPECT_EQ(stdoutOf(std::string(kCell) +
        "  public static method Main(): int "
        "    var a := new Cell(1); "
        "    using var b := new Cell(1); "
        "    var s := new Set<Cell?>(); "
        "    s.Add(a); s.Add(b); "
        "    Console.WriteLine(s.Count); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "1\n");
}

// BASE-GREEN CONTROL (2 of 2) — the §28.11 runtime-managed slot pairing, the
// check a conformance-corpus case cannot express (the corpus decides an
// accept/reject verdict and a stdout value, not a leak count). A `string?`
// element is co-owned by the collection: the insert takes an INDEPENDENT +1
// per element (given back when the collection frees the slot), and a REJECTED
// DUPLICATE takes none. Over 200 iterations, a wrong fix that retained on the
// duplicate path leaks one +1 per iteration and a wrong fix that did not
// retain at all over-releases at the free (a use-after-free). Green on the
// base (the base never deduped, so it retained once per appended copy and
// released once per stored copy); RED under either wrong fix.
BEZEL_TEST(set_nullable_element_dedup_9277,
           nullable_string_element_retain_is_paired_over_a_loop) {
    expectAsanClean(
        "module M; import System; class C "
        "  public static method Main(): int "
        "    var n := 0; "
        "    for var i in 0..<200 do "
        "      var s := new Set<string?>(); "
        "      s.Add(\"x\" + i.ToString()); "
        "      s.Add(\"x\" + i.ToString()); "
        "      n := n + s.Count; "
        "    end for; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C;",
        0);
}
