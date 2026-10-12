// fix §22.6 *`owned` and overload resolution* (v0.334): stage-4
// OWNERSHIP-MODE APPLICABILITY for a pair of overloads differing only by
// ordinary `owned`.
//
// SPEC: §22.6 (v0.334), pipeline stage 4 and its four exhaustive clauses:
//   1. "An owned production is applicable only to the `owned` candidate: a
//       move-rooted argument (`move x` ...) or any other production the §28.3
//       producers table classifies owned — a fresh `new`, an `owned`-returning
//       call, and kin."
//   2. "A borrow is applicable only to the plain candidate: an unmarked read of
//       a named binding, a storage/property/indexer read, a plain-returning
//       call."
//   3. "A context-typed production is applicable to both" (`null`, a
//       null-producing `default`) ... "the call falls to the ordinary ambiguity
//       rule".
//   4. "An argument with no ownership classification is applicable to both" ...
//       "both fall to the same ambiguity rule with the same remedy naming."
// plus: "The clause split is decidable from the argument alone" and "at most
// one member of a pair survives applicability at any argument carrying a
// classification, so the exact-match ranking ... operate on the surviving set
// exactly as written."
// The spec's own worked example:
//   r.Append(w);            // borrow — the plain overload; w untouched
//   r.Append(move w);       // transfer — the owned overload; w moved-from
//   r.Append(new Widget()); // fresh producer — the owned overload
//
// PRE-FIX BEHAVIOUR: BOTH example rows were rejected with
//   "ambiguous call to `Append`: no overload is unambiguously better (§22.6)"
// and the `move` row additionally drew
//   "the `move` call-site marker is only valid on an argument to an `owned`
//    parameter; this parameter does not take ownership" (§22.3)
// because the resolver fell through to first-in-source, which is the PLAIN
// member — the wrong member for a transfer.
//
// THE FOUR CELLS. This test pins all four, because a fix that merely
// "prefers the plain overload" would make rows 1 and 3 pass while silently
// KEEPING BORROWING in row 2 — a program that should transfer would borrow,
// and the caller would keep a binding the callee believes it owns. A negative
// cell that cannot fail pins nothing, so each negative below asserts a
// distinct, specific diagnostic rather than merely "some error":
//
//   cell 1  unmarked read `w`    -> plain member   (spec clause 2)
//   cell 2  move-rooted `move w` -> owned member   (spec clause 1)
//   cell 3  unmarked read must NOT consume `w`     (K3.13 no-silent-consume)
//   cell 4  move-rooted must NOT leave `w` alive   (§28.3 consume-table
//                                                   post-state)
//
// Rows 3 and 4 are the soundness pair: what makes cell 1 a BORROW is that `w`
// survives it, and what makes cell 2 a TRANSFER is that `w` does not. Assert
// survival and non-survival separately so a regression in either direction is
// visible, rather than both showing as one green compile.
//
// FAIL-WITHOUT (on d2a871ca): rows 1, 2 and 5 are rejected as ambiguous, so
// they never run and their stdout assertions cannot hold; rows 3 and 4 are
// rejected as ambiguous before any ownership check, so the "w survived" /
// "w was already moved out" assertions cannot hold either. Every row in this
// file therefore fails without the fix, for a reason specific to that row.

#include <cstdio>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasError(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// The AOT oracle captures the program's stdout in `stdoutText`; a marker is
// present when the substring appears anywhere in it.
bool stdoutHas(const JitResult& r, const char* sub) {
    return r.stdoutText.find(sub) != std::string::npos;
}

void expectRejected(const JitResult& r, const char* diag) {
    if (!hasError(r, diag)) {
        std::fprintf(stderr, "  expected diagnostic containing: %s\n", diag);
        for (const auto& d : r.diagnostics.diagnostics())
            std::fprintf(stderr, "    got: %s\n", d.message.c_str());
    }
    BEZEL_EXPECT_TRUE(hasError(r, diag));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The pair under test, plus the two cells that need a distinguishable tail.
// `w` is a `using var` so a cell-1 borrow must leave it usable, and a `var` so
// a cell-2 transfer must consume it.
constexpr const char* kPairPrelude =
    "public class Widget public constructor() end constructor; end Widget; "
    "public class Registry public constructor() end constructor; "
    "  public method Append(item: Widget): string => \"borrow\"; "
    "  public method Append(owned item: Widget): string => \"take\"; "
    "end Registry; ";

std::string withPair(const std::string& body) {
    return std::string("module M; ") + kPairPrelude + body;
}

} // namespace

// Cell 1 (POSITIVE, spec §22.6 clause 2 + example line 1): an unmarked read of
// a named binding is a BORROW, so only the PLAIN member is applicable and the
// call resolves to it. Prints "borrow".
BEZEL_TEST(owned_overload_applicability_22_6,
           unmarked_read_selects_plain_member_spec_22_6_clause_2) {
    auto r = runAot(withPair(
        "public class Program public static method Main() "
        "  using var r := new Registry(); "
        "  using var w := new Widget(); "
        "  Console.WriteLine(r.Append(w)); "
        "end Main; end Program;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(stdoutHas(r, "borrow"));
}

// Cell 2 (POSITIVE, spec §22.6 clause 1 + example line 2): a move-rooted
// argument is an OWNED PRODUCTION, so only the `owned` member is applicable.
// Prints "take". This is the row whose pre-fix fallthrough to the plain member
// also produced the bogus §22.3 "marker does not take ownership" diagnostic.
BEZEL_TEST(owned_overload_applicability_22_6,
           move_rooted_selects_owned_member_spec_22_6_clause_1) {
    auto r = runAot(withPair(
        "public class Program public static method Main() "
        "  using var r := new Registry(); "
        "  var w := new Widget(); "
        "  Console.WriteLine(r.Append(move w)); "
        "end Main; end Program;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(stdoutHas(r, "take"));
}

// Cell 3 (NEGATIVE for cell 1 — K3.13's no-silent-consume rule, cited by
// §22.6): routing the unmarked read to the `owned` member would CONSUME `w`.
// Two reads of the same binding must therefore both compile and both print
// "borrow" — the first borrow leaves `w` untouched. Pre-fix this is rejected
// as ambiguous, so it cannot run at all.
BEZEL_TEST(owned_overload_applicability_22_6,
           unmarked_read_does_not_consume_spec_22_6_clause_2_negative) {
    auto r = runAot(withPair(
        "public class Program public static method Main() "
        "  using var r := new Registry(); "
        "  using var w := new Widget(); "
        "  Console.WriteLine(r.Append(w)); "
        "  Console.WriteLine(r.Append(w)); "
        "end Main; end Program;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(stdoutHas(r, "borrow"));
}

// Cell 4 (NEGATIVE for cell 2 — §28.3's consume-table post-state): routing the
// move-rooted argument to the PLAIN member would leave `w` alive, so the value
// would be owned twice — once by `w`, once by the callee. A second `move w`
// must therefore be rejected on the ownership pass. Pre-fix this is rejected as
// ambiguous, which is the WRONG diagnostic: it never reaches the ownership
// check that names the actual defect.
BEZEL_TEST(owned_overload_applicability_22_6,
           move_rooted_consumes_binding_spec_22_6_clause_1_negative) {
    auto r = runAot(withPair(
        "public class Program public static method Main() "
        "  using var r := new Registry(); "
        "  var w := new Widget(); "
        "  Console.WriteLine(r.Append(move w)); "
        "  Console.WriteLine(r.Append(move w)); "
        "end Main; end Program;"));
    expectRejected(r, "already moved out");
}

// Row 5 (spec §22.6 example line 3, and the paragraph that follows it): a
// fresh `new Widget()` is an owned production by the §28.3 producers table, so
// it reaches the `owned` member — NOT a call-bounded borrow of an unbound
// temporary. The spec calls this "the useful reading" and explains why: the
// borrow alternative "would hand a storing callee a reference that dies at the
// statement boundary".
BEZEL_TEST(owned_overload_applicability_22_6,
           fresh_new_selects_owned_member_spec_22_6_clause_1) {
    auto r = runAot(withPair(
        "public class Program public static method Main() "
        "  using var r := new Registry(); "
        "  Console.WriteLine(r.Append(new Widget())); "
        "end Main; end Program;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(stdoutHas(r, "take"));
}

// Row 6 (spec §22.6 clause 2, the storage-read half): `obj.Item` is an
// `owned` FIELD, and §28.3's producers table classifies a field access as
// BORROWED (source: `obj`) even when the field itself is owned — only
// `move obj.Item` is owned. So the unmarked field read reaches the PLAIN
// member. This is the cell that separates "the argument is owned" from "the
// argument is an owned PRODUCTION": the two are not the same test.
BEZEL_TEST(owned_overload_applicability_22_6,
           field_read_selects_plain_member_spec_22_6_clause_2_storage_read) {
    auto r = runAot(withPair(
        "public class Box public owned Item: Widget; "
        "  public constructor() Item := new Widget(); end constructor; "
        "end Box; "
        "public class Program public static method Main() "
        "  using var r := new Registry(); "
        "  using var b := new Box(); "
        "  Console.WriteLine(r.Append(b.Item)); "
        "end Main; end Program;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(stdoutHas(r, "borrow"));
}

// Row 7 (spec §22.6 clause 1 via the §28.3 announcement table): a call whose
// declared return is `owned Widget` is an OWNED PRODUCTION (row 1), so it
// reaches the `owned` member. Paired with row 8 below, this shows the call
// case splits on the CALLEE's return marker, not on the call's shape — both
// rows are call expressions written identically apart from the declaration.
BEZEL_TEST(owned_overload_applicability_22_6,
           owned_returning_call_selects_owned_member_spec_22_6_clause_1) {
    auto r = runAot(withPair(
        "public class Factory public constructor() end constructor; "
        "  public method Make(): owned Widget => new Widget(); "
        "end Factory; "
        "public class Program public static method Main() "
        "  using var r := new Registry(); "
        "  using var f := new Factory(); "
        "  Console.WriteLine(r.Append(f.Make())); "
        "end Main; end Program;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(stdoutHas(r, "take"));
}

// Row 8 (spec §22.6 clause 2, the plain-returning-call half): announcement-
// table row 4 classifies a plain class-typed call result as BORROWED (sourced
// at its receiver), so it reaches the PLAIN member.
BEZEL_TEST(owned_overload_applicability_22_6,
           plain_returning_call_selects_plain_member_spec_22_6_clause_2) {
    // Written out in full rather than through `withPair`, because this row
    // needs a member ON `Widget` and the shared prelude closes that class.
    auto r = runAot(
        "module M; "
        "public class Widget public constructor() end constructor; "
        "  public method Self(): Widget => this; "
        "end Widget; "
        "public class Registry public constructor() end constructor; "
        "  public method Append(item: Widget): string => \"borrow\"; "
        "  public method Append(owned item: Widget): string => \"take\"; "
        "end Registry; "
        "public class Program public static method Main() "
        "  using var r := new Registry(); "
        "  using var w := new Widget(); "
        "  Console.WriteLine(r.Append(w.Self())); "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(stdoutHas(r, "borrow"));
}

// Row 9 (spec §22.6 clause 4, the "both applicable" cell): an argument with NO
// ownership classification is applicable to BOTH members, and §22.6 is
// explicit that this "fall[s] to the same ambiguity rule with the same remedy
// naming". So the compiler's ambiguity diagnostic is the CORRECT answer for
// such a shape, not a bug — and this row keeps a future change from "fixing" it
// by silently picking a side, which for an unclassified argument would be a
// silent transfer (or a silent borrow) with no §28.3 classification to justify
// it.
//
// The row pins the NEGATIVE property that is stable today: this shape must not
// be routed by the marker. MEASURED on d2a871ca: a bottom-typed `throw`
// argument at a pair position is ACCEPTED by the base compiler too (it lowers
// to a statement that emits nothing, #3079), so this is NOT a behaviour this
// fix changes and NOT one this file may assert a flip for. What the row does
// assert is that the classification predicate leaves the argument alone — the
// call is not reported as an `owned`/plain marker violation — so the unowned
// argument is never silently driven down the transfer path.
BEZEL_TEST(owned_overload_applicability_22_6,
           unclassified_argument_is_not_routed_by_marker_spec_22_6_clause_4) {
    auto r = runAot(
        "module M; "
        "public class Boom: Exception public constructor() end constructor; "
        "end Boom; "
        "public class Widget public constructor() end constructor; end Widget; "
        "public class Registry public constructor() end constructor; "
        "  public method Append(item: Widget): string => \"borrow\"; "
        "  public method Append(owned item: Widget): string => \"take\"; "
        "end Registry; "
        "public class Program public static method Main() "
        "  using var r := new Registry(); "
        "  Console.WriteLine(r.Append(throw new Boom())); "
        "end Main; end Program;");
    // No marker/ownership violation may be attributed to a bottom-typed
    // argument: it carries no ownership classification, so neither member is
    // more applicable than the other and neither may be selected by the marker.
    BEZEL_EXPECT_FALSE(hasError(r, "does not take ownership"));
    BEZEL_EXPECT_FALSE(hasError(r, "only valid on an argument"));
}