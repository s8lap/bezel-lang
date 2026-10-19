// Sema — #6389 / stdlib §4.12 + §5 (v0.338).
//
// THE DEFECT IS A SILENT-ACCEPTANCE INVERSION, NOT A LEAK. The issue this came
// from (#6389) reported a leak — `.Length` on a lazy LINQ source strands the
// materialized snapshot, 74,336 B / 707 allocations over 101 iterations. That
// leak is real, but it is a SYMPTOM. The larger defect is that the read is
// ACCEPTED at all, and that when accepted it produces a WRONG VALUE.
//
// STDLIB §4.12 declares `IEnumerable<out T>` with exactly one member:
//
//     public interface IEnumerable<out T>
//       method GetEnumerator(): owned IEnumerator<T>;
//     end IEnumerable;
//
// and the element count is §5's `public method Count(): int` — an operator
// METHOD, spelled with parentheses. So `e.Count()` compiles and returns 3;
// `e.Count` and `e.Length` have no member to resolve.
//
// MEASURED on this fix's parent (`r30-c1` @ b1a94683), over
// `f.Make().Where(x => x > 0)` on [1,2,3] — the answer is 3 in every case:
//
//   .Length  ->  1   codegen's string-`.Length` fallback claimed the receiver
//                    and GEP'd the `bz_lazy_t*` as a `bz.string*`, reading field
//                    1. Verified in the emitted IR:
//                      %33 = getelementptr inbounds nuw %bz.string, ptr %linq.owncl, i32 0, i32 1
//                      %str.len = load i64, ptr %33
//                    — the LAZY STRUCT's first field read as a string length.
//   .Count   ->  0   no arm claimed it; the read degraded to the #1386/#2434
//                    zero backstop with a Warning.
//
// Both emit a wrong answer and exit 0. Neither is a rejection.
//
// WHY A REJECT IS THE RIGHT FIX AND NOT A LOWERING. The precedent is directly
// adjacent in the same sema block: `isCountOnlyCollection` rejects `.Length` on
// `ICollection<T>` / `IList<T>` / `IReadOnlyList<T>` / `IReadOnlyCollection<T>`
// — the four heads that DO declare `Count` and no `Length` (stdlib §4.1) — with
// "use `Count`". Here NEITHER name is declared, so the remedy names the §5
// operator WITH ITS PARENTHESES, which is the distinction a reader needs:
// `e.Count()` compiles and returns 3, `e.Count` does not compile.
//
// Rejecting also removes the leak rather than papering over it: an expression
// that does not compile emits no `bz_lazy_own_source` / `bz_lazy_where` chain,
// so the 74,336 B cannot be stranded. A codegen-side reclaim alone would have
// fixed the byte count and left both wrong values in place — the leak was never
// the larger defect.
//
// FOUR FACES, TWO RECEIVER SHAPES, TWO PUNCTUATIONS. The shape is the part that
// is easy to get wrong, and it is why this file has four reject rows rather
// than one:
//
//   * LAZY SENTINEL — a deferred §5 chain (`f.Make().Where(p)`). Named TypeRef,
//     `declaration == nullptr`, `astNode == nullptr`, element in `genericArgs[0]`.
//     It carries NO NAME AT ALL, so the block's pre-existing `isBuiltin` gate
//     (which reads `astNode->name->parts`) is false for it.
//   * NAMED interface — an interface-typed local / parameter / field
//     (`var e: IEnumerable<int> := …`). Carries the head name in `astNode->name`,
//     so `isBuiltin` IS true and the arm can key on the name.
//   and each shape has both a bare `.` and a null-conditional `?.` spelling,
//   which reach SEPARATE sema blocks.
//
// MEASURED discriminator: an arm written against the NAMED shape alone does not
// fire on the issue's own repro, and an arm written against the LAZY shape
// alone does not fire on an interface-typed local. Both are pinned here.
//
// THE `importedName.empty()` CLAUSE IS LOAD-BEARING. A builtin interface
// RECONSTRUCTED FROM MODULE METADATA has the same Named / no-declaration /
// no-astNode shape but carries its name in `importedName`; that receiver is the
// #995 vtable object, NOT a `bz_lazy_t*`. Dropping the clause would reject a
// legal cross-module `IEnumerable<T>` local.
//
// This file does NOT pin that clause with a row — an earlier version of this
// comment claimed it did, and that was false: every row here is single-module,
// so a same-module `IEnumerable<int>` local carries an `astNode` and never
// reaches the imported shape. The clause's load-bearingness was established by
// DELETING it and observing the false reject of a legal two-module program, not
// by a test. Pinning it needs a cross-module row, which cannot be written as a
// single-module test for the reason given above; see
// `tests/module/imported_builtin_generic_not_lazy_2593_test.cpp` for the
// two-file harness shape. Filed as #6504.
//
// ORACLE. `runAot` with the diagnostic ASSERTED, not merely "it failed": a test
// that only asserts failure passes on any compile error at all, including an
// unrelated one this change introduced. Each row names its own needle. The
// control rows assert the POSITIVE — that the shapes which must keep compiling
// do, with the right VALUES — so a fix that over-rejected is caught by the same
// file and the same oracle. This is the direction that matters here, because the
// fix's own first draft over-rejected: gating on the `isCountOnlyCollection`
// block's `callee AND some extension exists` shape made `e.Count()` fail too,
// since §5's LINQ operators resolve through the LINQ typing and NOT through the
// §26.5 extension table. The correct discriminator is the CALLEE SLOT
// (`&ma != calleeOfCallCtx_`), and the `count_*_with_parens_still_works` rows
// below are what caught that draft.
//
// These rows use the plain AOT oracle, not `runAotChecked`: they are REJECT
// rows with no generated code to sanitize, and a leak assertion here would be
// vacuous by construction — there is nothing to leak once the expression is
// rejected.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// A `owned Array<int>` producer, so every row differs only in the member read.
constexpr const char* kPreamble =
    "module M; "
    "public class F "
    "  public method Make(): owned Array<int> "
    "    return [1, 2, 3]; "
    "  end Make; "
    "end F; ";

JitResult compile(const std::string& prog) { return runAot(prog); }

// Assert the program is REJECTED, and that the diagnostic is the one this fix
// introduced. `!r.ran` alone would pass on any compile error at all.
void expectRejected(const std::string& body, const std::string& needle) {
    JitResult r = compile(std::string(kPreamble) + body);
    BEZEL_EXPECT_FALSE(r.ran);
    // The driver rejected before executing it (never a crash / timeout / ASan
    // report), AND the diagnostic is the one this fix introduced. Asserting
    // only `!r.ran` would pass on any compile error at all.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle.c_str()));
}

// Assert the program COMPILES and exits with `value` — the affirmative form.
void expectValue(const std::string& body, int value) {
    JitResult r = compile(std::string(kPreamble) + body);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, value);
}

}  // namespace

// ===========================================================================
// THE REJECTIONS — four faces: lazy-sentinel / named, bare / `?.`.
// ===========================================================================

// FACE 1a — the issue's own repro, bare spelling. The LAZY SENTINEL shape.
BEZEL_TEST(ienumerable_count_length_member_reject_6389,
           lazy_chain_length_is_rejected_spec_4_12) {
    expectRejected(
        "public class M public static method Main(): int "
        "  var f := new F(); "
        "  var n := f.Make().Where(lambda(x) => x > 0).Length; "
        "  return n; "
        "end Main; end M;",
        "no member `Length` on a lazy LINQ sequence");
}

// FACE 1b — the same shape, the other member. On the parent this read
// compiled to 0 via the #1386/#2434 zero backstop and exited 0.
BEZEL_TEST(ienumerable_count_length_member_reject_6389,
           lazy_chain_count_is_rejected_spec_4_12) {
    expectRejected(
        "public class M public static method Main(): int "
        "  var f := new F(); "
        "  var n := f.Make().Where(lambda(x) => x > 0).Count; "
        "  return n; "
        "end Main; end M;",
        "no member `Count` on a lazy LINQ sequence");
}

// FACE 2a — the NAMED interface shape, bare spelling. This row is the
// discriminator between the two shapes: it is what an arm keyed only on the
// lazy sentinel's nameless TypeRef cannot reject.
BEZEL_TEST(ienumerable_count_length_member_reject_6389,
           interface_typed_local_length_is_rejected_spec_4_12) {
    expectRejected(
        "public class M public static method Main(): int "
        "  var f := new F(); "
        "  var e: IEnumerable<int> := f.Make(); "
        "  var n := e.Length; "
        "  return n; "
        "end Main; end M;",
        "no member `Length` on `IEnumerable<T>`");
}

// FACE 2b — same shape, `Count`.
BEZEL_TEST(ienumerable_count_length_member_reject_6389,
           interface_typed_local_count_is_rejected_spec_4_12) {
    expectRejected(
        "public class M public static method Main(): int "
        "  var f := new F(); "
        "  var e: IEnumerable<int> := f.Make(); "
        "  var n := e.Count; "
        "  return n; "
        "end Main; end M;",
        "no member `Count` on `IEnumerable<T>`");
}

// FACE 3a/3b — the `?.` punctuation on the named shape. `?.` reaches a
// SEPARATE sema block (`resolveNullCondMember`), so fixing only the bare block
// leaves the null-conditional spelling compiling to the same wrong value.
BEZEL_TEST(ienumerable_count_length_member_reject_6389,
           null_conditional_length_is_rejected_spec_4_12) {
    expectRejected(
        "public class M public static method Main(): int "
        "  var f := new F(); "
        "  var e: IEnumerable<int>? := f.Make(); "
        "  var n := e?.Length; "
        "  return n; "
        "end Main; end M;",
        "no member `Length` on `IEnumerable<T>`");
}

BEZEL_TEST(ienumerable_count_length_member_reject_6389,
           null_conditional_count_is_rejected_spec_4_12) {
    expectRejected(
        "public class M public static method Main(): int "
        "  var f := new F(); "
        "  var e: IEnumerable<int>? := f.Make(); "
        "  var n := e?.Count; "
        "  return n; "
        "end Main; end M;",
        "no member `Count` on `IEnumerable<T>`");
}

// ===========================================================================
// THE CONTROLS — the over-reject direction, and the rows that caught this fix's
// own first draft. Every one is green on the parent too; their job is to be
// green HERE, i.e. to prove the four arms above did not take a shape with them.
// ===========================================================================

// THE remedy the diagnostics name: §5's operator WITH ITS PARENTHESES, on the
// LAZY SENTINEL shape, returning the right count. This is the row that
// distinguishes the CALLEE-SLOT gate from the block's usual
// "callee AND some §26.5 extension exists" shape — §5's LINQ operators are NOT
// in the extension table, so the latter rejects this program.
BEZEL_TEST(ienumerable_count_length_member_reject_6389,
           count_operator_with_parens_still_works_spec_5) {
    expectValue(
        "public class M public static method Main(): int "
        "  var f := new F(); "
        "  return f.Make().Where(lambda(x) => x > 0).Count(); "
        "end Main; end M;",
        3);
}

// The same on the NAMED interface shape rather than a chain.
BEZEL_TEST(ienumerable_count_length_member_reject_6389,
           count_operator_on_interface_typed_local_still_works_spec_5) {
    expectValue(
        "public class M public static method Main(): int "
        "  var f := new F(); "
        "  var e: IEnumerable<int> := f.Make(); "
        "  return e.Count(); "
        "end Main; end M;",
        3);
}

// `Array<T>.Count` — the CONCRETE collection's own property (stdlib §4.4). The
// shape the rejected reads are most easily confused with, and the one a
// too-broad receiver predicate would take.
BEZEL_TEST(ienumerable_count_length_member_reject_6389,
           array_count_property_still_works_spec_4_4) {
    expectValue(
        "public class M public static method Main(): int "
        "  var f := new F(); "
        "  return f.Make().Count; "
        "end Main; end M;",
        3);
}

// `String.Length` — the one genuine `Length` property (stdlib §9.6) — is
// untouched, including on a `+` producer where the string-release path fires.
BEZEL_TEST(ienumerable_count_length_member_reject_6389,
           string_length_property_still_works_spec_9_6) {
    JitResult r = compile(
        "module M; "
        "public class M public static method Main(): int "
        "  var s := \"ab\" + \"cde\"; "
        "  return s.Length; "
        "end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// The for-in desugaring (§29.2) consumes a lazy chain AND an interface-typed
// source; both are the primary legitimate uses of these types, so both must be
// untouched. Two rows, because they are two different receiver shapes.
BEZEL_TEST(ienumerable_count_length_member_reject_6389,
           for_in_over_a_lazy_chain_still_works_spec_29_2) {
    expectValue(
        "public class M public static method Main(): int "
        "  var f := new F(); "
        "  var acc := 0; "
        "  for var x in f.Make().Where(lambda(y) => y > 0) do "
        "    acc := acc + x; "
        "  end; "
        "  return acc; "
        "end Main; end M;",
        6);  // 1 + 2 + 3
}

BEZEL_TEST(ienumerable_count_length_member_reject_6389,
           for_in_over_an_interface_typed_source_still_works_spec_29_2) {
    expectValue(
        "public class M public static method Main(): int "
        "  var f := new F(); "
        "  var e: IEnumerable<int> := f.Make(); "
        "  var acc := 0; "
        "  for var x in e do acc := acc + x; end; "
        "  return acc; "
        "end Main; end M;",
        6);
}

// The pre-existing rejects in the SAME block must survive verbatim — a reword
// would orphan every lit needle and gtest expectation that cites them.
//
// `IReadOnlyList<T>.Length` keeps its own `use \`Count\`` message (stdlib
// §4.1) and must NOT be captured by the new `IEnumerable<T>` predicate.
BEZEL_TEST(ienumerable_count_length_member_reject_6389,
           ireadonlylist_length_keeps_its_own_reject_spec_4_1) {
    JitResult r = compile(
        "module M; "
        "public class M public static method Main(): int "
        "  var a: Array<int> := [1, 2, 3]; "
        "  var il: IReadOnlyList<int> := a; "
        "  return il.Length; "
        "end Main; end M;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `Length` on `IReadOnlyList<T>`: use `Count`"));
}

// `IReadOnlyCollection<T>.Count` keeps #853's diagnostic — a DIFFERENT reject
// with a DIFFERENT remedy ("call `.Count` on the concrete collection"). What
// this row pins is that the new arms did not capture this head: the needle is
// #853's message, not the new `IEnumerable<T>` one.
BEZEL_TEST(ienumerable_count_length_member_reject_6389,
           ireadonlycollection_count_keeps_its_own_reject_spec_4_1) {
    JitResult r = compile(
        "module M; "
        "public class M public static method Main(): int "
        "  var a: Array<int> := [1, 2, 3]; "
        "  var rc: IReadOnlyCollection<int> := a; "
        "  return rc.Count; "
        "end Main; end M;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "#853"));
}