// fix #8583 (spec §7.5, stdlib §4.1 / §4.2, v0.357): the `?.` spelling of a
// MUTATING member of a POLYMORPHIC collection interface must be REFUSED with
// the SAME diagnostic the `.` spelling gets — never silently dropped.
//
// THE RULES.
//   §7.5 (docs/Bezel_Language_Spec.md, v0.357) "`x?.M` returns null if `x` is
//   null; otherwise it performs the access. The result type is always
//   nullable." The `?.` spelling resolves the member exactly as the `.`
//   spelling does — so the two spellings of ONE member may not disagree.
//   stdlib §4.1 (v0.357) `ICollection<T>` declares `Add(item: T)` /
//   `Clear()` / `Contains(item: T): bool` / `Remove(item: T): bool`;
//   `IList<T>` inherits them and adds `Insert(index: int, item: T)`.
//   stdlib §4.2 (v0.357) the owning pair: `IOwningCollection<T>` declares
//   `Add(owned item: T)` / `Clear()` / `Contains` / `Remove`; `IOwningList<T>`
//   inherits them and adds `Insert(index: int, owned item: T)`.
//
// THE DEFECT (issue #8583, MEASURED by its reporter on staging).
//   `c?.Add(4)` / `c?.Clear()` / `c?.Remove(8)` on an `ICollection<T>?`
//   receiver were SILENTLY DROPPED: the `?.` spelling reached NO typing arm
//   in `ExprTC::resolveNullCondMember`, so the call stayed UNTYPED, codegen
//   lowered nothing, and the enclosing statement emitted NOTHING — the
//   program printed `3` (the array was untouched), exited 0, and produced no
//   diagnostic at all. The same held for the owning heads'
//   `Add` / `Clear` / `Remove` / `Insert`.
//
//   The `.` spelling of the SAME member is NOT silent: codegen's #853 fence
//   REFUSES `.Add` / `.Clear` / `.Remove` / `.Insert` through a polymorphic
//   collection interface (ICollection<T> / IReadOnlyCollection<T> /
//   IOwningCollection<T> / IOwningList<T>) — those interfaces are implemented
//   by collections with incompatible runtime representations, the concrete
//   backing is erased at the interface, and no single runtime dispatch is
//   sound. §7.5 says the `?.` spelling must agree with the `.` spelling: one
//   diagnostic for both. It must never vanish.
//
// THE FIX.
//   The `?.` spelling now REACHES the `.` spelling's own fence, so the two
//   spellings emit the SAME diagnostic — byte for byte, at the same source
//   location:
//     * `Remove` returns `bool`, so the `?.` access is typed `bool?` in sema
//       (the #7557 `Contains` pattern) and the `?.` lowering builds its own
//       synthetic `.` node, which lands in `tryLowerCollectionInstanceCall`
//       — where the `.` spelling's fence lives;
//     * `Add` / `Clear` / `Insert` return UNIT, which has no nullable form
//       for §7.5 to lift, so the `?.` lowering routes them through the fence
//       directly.
//
// Every row below is load-bearing: each one FAILS on the base commit (the
// statement was dropped: the program compiled, printed `3`, and exited 0)
// and PASSES with the fix.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The #853 fence's wording, as the `.` spelling emits it. The `?.` spelling
// now reaches the SAME fence, so the needle is the same string — only the
// member name differs.
std::string fenceNeedle(const char* member) {
    return std::string("`.") + member +
           "` through a polymorphic collection interface";
}

// A program that is REJECTED at compile time with the given diagnostic, and
// that ran nothing. (runAot synthesizes an Error diagnostic for a runtime
// crash too, so this checks the TEXT as well as the absence of a run.)
bool rejectedWith(const std::string& program, const std::string& needle) {
    JitResult r = runAot(program);
    if (r.ran || !r.stdoutText.empty()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// The issue's own repro shape: a collection-interface view over an Array.
std::string prog(const std::string& body) {
    return "module M; import System; import System.Linq; "
           "class P "
           "  public static method Main(): int " +
           body + " return 0; end Main; end P;";
}

// The owning variant (stdlib §4.5): an OwningArray viewed through the owning
// collection interfaces. `R` is a class element, so the owning collection
// disposes it at scope end.
std::string owningProg(const std::string& body) {
    return "module M; import System; import System.Linq; "
           "class R public V: int; "
           "  public constructor(v: int) V := v; end constructor; end R; "
           "class P "
           "  public static method Main(): int "
           "    using var a := new OwningArray<R>(); "
           "    a.Add(move (new R(1))); a.Add(move (new R(2))); " +
           body + " return 0; end Main; end P;";
}

void expectStdout(const std::string& program, const char* expected) {
    JitResult r = runAotChecked(program);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(expected));
}

}  // namespace

// ---------------------------------------------------------------------------
// THE ISSUE'S OWN REPRO — `?.Add` / `?.Clear` / `?.Remove` on
// `ICollection<T>?`.
// ---------------------------------------------------------------------------

// stdlib §4.1 / §7.5: `ICollection<int>?.Add(4)` is REFUSED, exactly as the
// `.` spelling `ICollection<int>.Add(4)` is (#853: the concrete backing is
// erased at the interface). Before the fix the statement was dropped: the
// program compiled, printed `3` (the array still held three elements),
// exited 0, and produced no diagnostic at all.
BEZEL_TEST(null_cond_collection_iface_mutators_8583,
           icol_add_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var c: ICollection<int>? := a; "
             "  c?.Add(4); "
             "  Console.WriteLine(a.Count); "),
        fenceNeedle("Add")));
}

// stdlib §4.1 / §7.5: the same for `?.Clear()`.
BEZEL_TEST(null_cond_collection_iface_mutators_8583,
           icol_clear_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var c: ICollection<int>? := a; "
             "  c?.Clear(); "
             "  Console.WriteLine(a.Count); "),
        fenceNeedle("Clear")));
}

// stdlib §4.1 / §7.5: and for `?.Remove(8)`. `Remove` returns `bool`, so the
// `?.` access is typed `bool?` in sema and the `?.` lowering reaches the
// fence through its own synthetic `.` node — the same route the `?.Contains`
// spelling takes since #7557.
BEZEL_TEST(null_cond_collection_iface_mutators_8583,
           icol_remove_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var c: ICollection<int>? := a; "
             "  c?.Remove(8); "
             "  Console.WriteLine(a.Count); "),
        fenceNeedle("Remove")));
}

// The EXPRESSION spelling of the same call: `c?.Remove(8) ?? false`. Before
// the fix the call was UNTYPED, so the `??` had nothing to lower and the
// whole statement was dropped behind the #3079 warning (exit 0, nothing
// printed). Now the `?.` spelling reaches the fence and the expression is
// refused exactly as the `.` spelling's `c.Remove(8) ?? false` is.
BEZEL_TEST(null_cond_collection_iface_mutators_8583,
           icol_remove_expression_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var c: ICollection<int>? := a; "
             "  Console.WriteLine(c?.Remove(8) ?? false); "),
        fenceNeedle("Remove")));
}

// ---------------------------------------------------------------------------
// THE OWNING HEADS (stdlib §4.2) — the same refusal, on `IOwningCollection`
// and `IOwningList`.
// ---------------------------------------------------------------------------

// stdlib §4.2 / §7.5: `IOwningCollection<R>?.Add(owned item)` is REFUSED,
// like the `.` spelling. Before the fix it was dropped silently: the program
// compiled, printed nothing, and exited 0.
BEZEL_TEST(null_cond_collection_iface_mutators_8583,
           iowningcoll_add_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        owningProg("  var oc: IOwningCollection<R>? := a; "
                   "  oc?.Add(move (new R(3))); "),
        fenceNeedle("Add")));
}

// stdlib §4.2 / §7.5: `IOwningCollection<R>?.Clear()` — same.
BEZEL_TEST(null_cond_collection_iface_mutators_8583,
           iowningcoll_clear_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        owningProg("  var oc: IOwningCollection<R>? := a; "
                   "  oc?.Clear(); "),
        fenceNeedle("Clear")));
}

// stdlib §4.2 / §7.5: `IOwningCollection<R>?.Remove(item)` — same.
BEZEL_TEST(null_cond_collection_iface_mutators_8583,
           iowningcoll_remove_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        owningProg("  var oc: IOwningCollection<R>? := a; "
                   "  oc?.Remove(a[0]); "),
        fenceNeedle("Remove")));
}

// stdlib §4.2 / §7.5: `IOwningList<R>?.Add(owned item)` — the owning LIST
// interface is polymorphic over the owning backings, so its `.` spelling is
// refused too.
BEZEL_TEST(null_cond_collection_iface_mutators_8583,
           iowninglist_add_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        owningProg("  var ol: IOwningList<R>? := a; "
                   "  ol?.Add(move (new R(3))); "),
        fenceNeedle("Add")));
}

// stdlib §4.2 / §7.5: `IOwningList<R>?.Clear()` — same.
BEZEL_TEST(null_cond_collection_iface_mutators_8583,
           iowninglist_clear_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        owningProg("  var ol: IOwningList<R>? := a; "
                   "  ol?.Clear(); "),
        fenceNeedle("Clear")));
}

// stdlib §4.2 / §7.5: `IOwningList<R>?.Remove(item)` — same.
BEZEL_TEST(null_cond_collection_iface_mutators_8583,
           iowninglist_remove_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        owningProg("  var ol: IOwningList<R>? := a; "
                   "  ol?.Remove(a[0]); "),
        fenceNeedle("Remove")));
}

// stdlib §4.2 / §7.5: `IOwningList<R>?.Insert(index, owned item)` — `Insert`
// is declared on the owning LIST interface only (stdlib §4.2), and its `.`
// spelling is refused by the same #853 fence.
BEZEL_TEST(null_cond_collection_iface_mutators_8583,
           iowninglist_insert_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        owningProg("  var ol: IOwningList<R>? := a; "
                   "  ol?.Insert(0, move (new R(3))); "),
        fenceNeedle("Insert")));
}

// ---------------------------------------------------------------------------
// CONTROL 1 — the `.` spelling keeps its refusal, byte for byte.
//
// The two spellings of ONE member must agree (§7.5). This row pins that the
// `.` spelling is refused with the SAME text the `?.` spelling now gets, and
// that the fix did not disturb the `.` path. It goes RED under a wrong fix
// that changed the fence's wording or the `.` spelling's route.
// ---------------------------------------------------------------------------
BEZEL_TEST(null_cond_collection_iface_mutators_8583,
           dot_spelling_add_still_refused_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var c: ICollection<int> := a; "
             "  c.Add(4); "
             "  Console.WriteLine(a.Count); "),
        fenceNeedle("Add")));
}

// ---------------------------------------------------------------------------
// CONTROL 2 — the borrowing LIST interface is NOT refused.
//
// `IList<T>` is the array-backed borrowing list interface: the `.` spelling
// of its members PERFORMS them (the bz_array_* bridge), so the `?.` spelling
// must perform them too — it must NOT be refused. This row goes RED under a
// wrong fix whose receiver predicate covered the borrowing list interfaces
// (the over-broad reading of "polymorphic collection interface").
// ---------------------------------------------------------------------------
BEZEL_TEST(null_cond_collection_iface_mutators_8583,
           ilist_contains_still_performed_spec_7_5) {
    expectStdout(
        prog("  var a: Array<int> := [1, 2, 3]; "
             "  var il: IList<int>? := a; "
             "  Console.WriteLine(il?.Contains(2) ?? false); "),
        "True\n");
}
