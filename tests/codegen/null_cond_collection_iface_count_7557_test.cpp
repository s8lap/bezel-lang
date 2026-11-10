// fix #7557 (spec §7.5, stdlib §4.1 / §4.2, v0.357): a `?.` member access on
// a built-in COLLECTION INTERFACE receiver must either perform the access or
// be a diagnostic — never be silently dropped.
//
// THE RULES.
//   §7.5 (docs/Bezel_Language_Spec.md, v0.357) "`x?.M` returns null if `x` is
//   null; otherwise it performs the access. The result type is always
//   nullable." The `?.` spelling resolves the member exactly as the `.`
//   spelling does.
//   stdlib §4.1 (v0.357) `IReadOnlyCollection<out T>` declares `pure readonly
//   property Count: int` and `GetEnumerator`; `IReadOnlyList<T>` inherits it
//   and adds the indexer; `ICollection<T>` declares `Count` / `IsReadOnly` /
//   `Add` / `Clear` / `Contains` / `Remove` / `GetEnumerator`; `IList<T>`
//   inherits `ICollection<T>` and adds `Insert` and the settable indexer.
//   stdlib §4.2 the owning pair: `IOwningCollection<T>` / `IOwningList<T>`.
//
// THE DEFECT (issue #7557, MEASURED by its reporter on staging).
//   `ICollection<T>?.Count` and `IReadOnlyCollection<T>?.Count` were dropped
//   in the same way the reporter's `ro?.Contains(x)` was: the `?.` spelling
//   reached NO typing arm in resolveNullCondMember, so the access stayed
//   UNTYPED, codegen lowered nothing, and the enclosing statement emitted
//   nothing at all — signalled only by the `internal:` #3079 "failed to
//   lower" warning, with the program exiting 0. The `?.` spelling of
//   `Contains` on those heads, and of both members on `IOwningCollection<T>` /
//   `IOwningList<T>`, dropped the same way.
//
// THE FIX. The `.` spelling of the same accesses is NOT silent:
//   * `.Count` / `.Contains` through a POLYMORPHIC collection interface
//     (ICollection<T> / IReadOnlyCollection<T> / IOwningCollection<T> /
//     IOwningList<T>) is REFUSED by codegen's #853 fence — those interfaces
//     are implemented by collections with incompatible runtime
//     representations (array / linked-list / hash-table), the concrete backing
//     is erased at the interface, and no single runtime dispatch is sound;
//   * `.Count` through the array-backed LIST interfaces (IList<T> /
//     IReadOnlyList<T> / IOwningList<T>) is PERFORMED (the bz_array_* bridge).
// The `?.` spelling now reaches the SAME fences: the `?.` lowering routes the
// access through the `.` spelling's own dispatch, so the two spellings agree —
// one diagnostic for both, or one value for both.
//
// Every row below is load-bearing: each one fails on the base commit (the
// access was dropped: the program compiled with a #3079 warning, printed
// nothing, and exited 0) and passes with the fix.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The #853 fence's wording, as the `.` spelling emits it. The `?.` spelling
// now reaches the SAME fence, so the needle is the same string.
const char* kCountFence =
    "`.Count` through a polymorphic collection interface";
const char* kContainsFence =
    "`.Contains` through a polymorphic collection interface";

// A program that is REJECTED at compile time with the given diagnostic, and
// that ran nothing. (runAot synthesizes an Error diagnostic for a runtime
// crash too, so this checks the TEXT as well as the absence of a run.)
bool rejectedWith(const std::string& program, const char* needle) {
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
// THE ISSUE'S OWN REPRO — `?.Count` on the polymorphic collection interfaces.
// ---------------------------------------------------------------------------

// stdlib §4.1 / §7.5: `ICollection<int>?.Count` is REFUSED, exactly as the
// `.` spelling `ICollection<int>.Count` is (#853: the concrete backing is
// erased at the interface). Before the fix the access was dropped: the
// program compiled with the #3079 "failed to lower" warning, printed NOTHING
// (not even a line terminator) and exited 0.
BEZEL_TEST(null_cond_collection_iface_7557,
           icol_count_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var c: ICollection<int>? := a; "
             "  Console.WriteLine(c?.Count ?? -1); "),
        kCountFence));
}

// stdlib §4.1 / §7.5: `IReadOnlyCollection<int>?.Count` is REFUSED, like the
// `.` spelling. (stdlib §4.1 declares `Count: int` on
// `IReadOnlyCollection<out T>` — the member EXISTS; the refusal is about the
// erased runtime backing, not about the name.)
BEZEL_TEST(null_cond_collection_iface_7557,
           irocol_count_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var rc: IReadOnlyCollection<int>? := a; "
             "  Console.WriteLine(rc?.Count ?? -1); "),
        kCountFence));
}

// stdlib §4.2 / §7.5: the owning polymorphic head is refused the same way.
BEZEL_TEST(null_cond_collection_iface_7557,
           iowningcoll_count_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        owningProg("  var oc: IOwningCollection<R>? := a; "
                   "  Console.WriteLine(oc?.Count ?? -1); "),
        kCountFence));
}

// The LOCAL-INITIALIZER spelling of the same access: `var n: int? := c?.Count;`
// degraded to a null `n` behind the #2434 warning (the reporter saw a wrong
// value where §7.5 requires the access or a diagnostic). It is refused now.
BEZEL_TEST(null_cond_collection_iface_7557,
           icol_count_local_initializer_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var c: ICollection<int>? := a; "
             "  var n: int? := c?.Count; "
             "  Console.WriteLine(n ?? -1); "),
        kCountFence));
}

// ---------------------------------------------------------------------------
// THE ISSUE'S OWN REPRO — `?.Contains` on the polymorphic collection
// interfaces (the member #7557's title names, on the heads that reject it).
// ---------------------------------------------------------------------------

// stdlib §4.1 / §7.5: `ICollection<int>?.Contains(8)` is REFUSED, like the
// `.` spelling `ICollection<int>.Contains(8)` (#853). Before the fix it was
// dropped: a #3079 warning, nothing printed, exit 0.
BEZEL_TEST(null_cond_collection_iface_7557,
           icol_contains_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var c: ICollection<int>? := a; "
             "  Console.WriteLine(c?.Contains(8) ?? false); "),
        kContainsFence));
}

// stdlib §4.1 / §7.5: the same for the read-only polymorphic head.
BEZEL_TEST(null_cond_collection_iface_7557,
           irocol_contains_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var rc: IReadOnlyCollection<int>? := a; "
             "  Console.WriteLine(rc?.Contains(8) ?? false); "),
        kContainsFence));
}

// stdlib §4.2 / §7.5: and for the owning polymorphic head.
BEZEL_TEST(null_cond_collection_iface_7557,
           iowningcoll_contains_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        owningProg("  var oc: IOwningCollection<R>? := a; "
                   "  Console.WriteLine(oc?.Contains(a[0]) ?? false); "),
        kContainsFence));
}

// stdlib §4.2 / §7.5: `IOwningList<R>` is an OWNING list interface — the
// `.` spelling of `.Contains` on it is refused (#853: the owning list
// interfaces are polymorphic over the owning backings), so the `?.` spelling
// is refused the same way. Before the fix it was dropped silently.
BEZEL_TEST(null_cond_collection_iface_7557,
           iowninglist_contains_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        owningProg("  var ol: IOwningList<R>? := a; "
                   "  Console.WriteLine(ol?.Contains(a[0]) ?? false); "),
        kContainsFence));
}

// ---------------------------------------------------------------------------
// THE MEMBER THAT IS PERFORMED — `IOwningList<R>?.Count`.
// ---------------------------------------------------------------------------

// stdlib §4.2 / §7.5: `IOwningList<R>` is an array-backed LIST interface, so
// the `.` spelling of `.Count` on it is PERFORMED (the bz_array_* bridge), and
// the `?.` spelling must perform it too: the result is `int?` (§7.5: the
// result type is always nullable), null for a null receiver.
//
// Before the fix this access reached no typing arm, so it was dropped: a
// #3079 warning, nothing printed, exit 0.
BEZEL_TEST(null_cond_collection_iface_7557,
           iowninglist_count_lowered_spec_7_5) {
    expectStdout(
        owningProg("  var ol: IOwningList<R>? := a; "
                   "  Console.WriteLine(ol?.Count ?? -1); "),
        "2\n");
}

// §7.5 Evaluation: a null receiver yields null (the access does not occur).
BEZEL_TEST(null_cond_collection_iface_7557,
           iowninglist_count_null_receiver_spec_7_5) {
    expectStdout(
        owningProg("  var ol: IOwningList<R>? := null; "
                   "  Console.WriteLine(ol?.Count ?? -1); "),
        "-1\n");
}

// ---------------------------------------------------------------------------
// NO REGRESSION — the list interfaces that already worked keep working, and
// the issue's FIRST half (`ro?.Contains(x)` on `IReadOnlyList<int>?`, fixed by
// #7544) stays fixed.
// ---------------------------------------------------------------------------

// The issue's literal repro: `ro?.Contains(2)` on an `IReadOnlyList<int>?`.
// §7.5 / stdlib §4.1: the call happens and answers true.
BEZEL_TEST(null_cond_collection_iface_7557,
           issue_repro_ro_contains_no_regression_spec_7_5) {
    expectStdout(
        prog("  var a: Array<int> := [1, 2, 3]; "
             "  var ro: IReadOnlyList<int>? := a; "
             "  Console.WriteLine(ro?.Contains(2) ?? false); "),
        "True\n");
}

// §7.5 / stdlib §4.1: `IList<int>?.Count` and `IList<int>?.Contains(x)` —
// the array-backed list interface the `.` spelling performs — keep performing
// under the `?.` spelling, and a null receiver still yields null.
BEZEL_TEST(null_cond_collection_iface_7557,
           ilist_count_contains_no_regression_spec_7_5) {
    expectStdout(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var il: IList<int>? := a; "
             "  var nl: IList<int>? := null; "
             "  Console.WriteLine(il?.Count ?? -1); "
             "  Console.WriteLine(il?.Contains(8) ?? false); "
             "  Console.WriteLine(nl?.Count ?? -1); "
             "  Console.WriteLine(nl?.Contains(8) ?? true); "),
        "3\nTrue\n-1\nTrue\n");
}

// §7.5 / stdlib §4.1: `IReadOnlyList<int>?.Count` keeps performing.
BEZEL_TEST(null_cond_collection_iface_7557,
           irocolist_count_no_regression_spec_7_5) {
    expectStdout(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var rol: IReadOnlyList<int>? := a; "
             "  Console.WriteLine(rol?.Count ?? -1); "),
        "3\n");
}

// stdlib §4.12 / §7.5: `IEnumerable<int>` declares no `Count` at all (the
// count is the §5 `Count()` operator), so `e?.Count` is a diagnostic — as it
// was before this fix. The `?.` spelling must not start admitting it.
BEZEL_TEST(null_cond_collection_iface_7557,
           ienumerable_count_stays_rejected_spec_stdlib_4_12) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var e: IEnumerable<int>? := a; "
             "  Console.WriteLine(e?.Count ?? -1); "),
        "no member `Count` on `IEnumerable<T>`"));
}
