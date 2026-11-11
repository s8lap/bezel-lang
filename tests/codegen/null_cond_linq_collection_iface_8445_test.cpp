// fix #8445 (spec §7.5, stdlib §5 / §4.1, v0.357): the `?.` spelling of a
// stdlib §5 LINQ OPERATOR on a POLYMORPHIC collection interface
// (`ICollection<T>` / `IReadOnlyCollection<T>` / `IOwningCollection<T>`) must
// be REFUSED exactly as the `.` spelling is refused — never silently dropped.
//
// THE RULES.
//   §7.5 (docs/Bezel_Language_Spec.md, v0.357) "`x?.M` returns null if `x` is
//   null; otherwise it performs the access. The result type is always
//   nullable." — and the `?.` spelling resolves the member exactly as the `.`
//   spelling does, so the two spellings of ONE member may not disagree.
//   stdlib §4.1 (v0.357) `IReadOnlyCollection<out T>: IEnumerable<T>` and
//   `ICollection<T>: IReadOnlyCollection<T>` are the BORROWING polymorphic
//   collection interfaces; stdlib §4.2 adds the owning `IOwningCollection<T>`.
//   stdlib §5 (v0.357) `extension IEnumerable<T>` declares the operator
//   surface (`Sum` / `Any` / `All` / `First` / `Last` / `Single` / `Min` /
//   `Max` / `Average` / `ToArray` / `Where` / `Select` / `Take` / `Skip` /
//   `OrderBy` / `GroupBy` / `ToDictionary` / …).
//
// THE DEFECT (issue #8445, MEASURED by its reporter).
//   The `.` spelling of `c.Sum()` on an `ICollection<int>` is REFUSED by
//   codegen's #2457 backstop — the polymorphic collection interfaces ERASE the
//   concrete backing (array / linked-list / hash-table), so no §5 operator is
//   sound on them and this compiler has no lowering arm for one:
//     "`Sum` on built-in type `ICollection` produced no value, and no dispatch
//      arm emitted any code for this call. …"
//   The `?.` spelling of the SAME call was SILENTLY DROPPED instead: sema's
//   §7.5 twin types a `?.` call by re-entering the `.` typing on a placeholder
//   receiver, and stdlib §5's operator arm deliberately EXCLUDES
//   `ICollection<T>` / `IReadOnlyCollection<T>` (the #7544 note: "their runtime
//   representation is erased (codegen rejects §5 operators on them loudly,
//   #853), and typing the call would turn that loud rejection into a silent
//   drop"). So the `?.` call stayed UNTYPED, the `?.` lowering had nothing to
//   lower, the enclosing statement was dropped behind the #3079 warning, and
//   the program printed NOTHING and exited 0.
//
// THE FIX. The `?.` spelling is routed through the `.` spelling's OWN
//   dispatch, so the two spellings of ONE member cannot disagree: the `?.`
//   lowering lowers a synthetic `.` call over the receiver, and the `.`
//   spelling's own #2457 backstop refuses it. The refusal is BYTE-IDENTICAL
//   to the `.` spelling's, produced by the SAME code — the `same_message`
//   test below pins that, not merely "some Error".
//
// Every row below is load-bearing: each one fails on the base commit (the
// `?.` spelling was dropped: the program compiled with a #3079 warning,
// printed nothing, and exited 0) and passes with the fix.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The #2457 backstop's wording, as the `.` spelling emits it. The `?.`
// spelling now reaches the SAME backstop, so the needle is the same string.
const char* kNoValueOnBuiltIn = "produced no value";

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

// The FIRST Error's MESSAGE, with the location prefix stripped.
//
// The AOT harness reports a compile REJECT as ONE Error diagnostic carrying
// the driver's RENDERED stderr ("<path>:<line>:<col>: <message>" per
// diagnostic — Driver.cpp's `renderDiagnostic`), and every `runAot` call
// writes its OWN temporary source file, so the location prefix differs
// between two runs of two programs. What the two spellings of ONE member must
// agree on is the MESSAGE, so the prefix is stripped before comparing.
std::string firstErrorMessage(const std::string& program) {
    JitResult r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        std::string blob = d.message;
        const std::size_t nl = blob.find('\n');
        if (nl != std::string::npos) blob = blob.substr(0, nl);
        const std::size_t at = blob.find(": ");
        return at == std::string::npos ? blob : blob.substr(at + 2);
    }
    return std::string();
}

// The issue's own repro shape: a collection-interface view over an Array.
std::string prog(const std::string& body) {
    return "module M; import System; import System.Linq; "
           "class P "
           "  public static method Main(): int " +
           body + " return 0; end Main; end P;";
}

// The owning variant (stdlib §4.2 / §4.5): an OwningArray viewed through the
// owning collection interface. `R` is a class element, so the owning
// collection disposes it at scope end.
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
// THE ISSUE'S OWN REPRO — `?.Sum()` on `ICollection<T>?`.
// ---------------------------------------------------------------------------

// stdlib §5 / §7.5: `ICollection<int>?.Sum()` is REFUSED, exactly as the `.`
// spelling `ICollection<int>.Sum()` is. Before the fix it was dropped: the
// program compiled with the #3079 "failed to lower" warning, printed NOTHING
// (not even a line terminator) and exited 0.
BEZEL_TEST(null_cond_linq_collection_iface_8445,
           icol_sum_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var c: ICollection<int>? := a; "
             "  Console.WriteLine(c?.Sum() ?? -1); "),
        kNoValueOnBuiltIn));
}

// The LOCAL-INITIALIZER spelling of the same call: `var r := c?.Sum();`
// degraded to a zero value behind the #2434 warning. It is refused now.
BEZEL_TEST(null_cond_linq_collection_iface_8445,
           icol_sum_local_initializer_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var c: ICollection<int>? := a; "
             "  var r := c?.Sum(); "
             "  Console.WriteLine(r ?? -1); "),
        kNoValueOnBuiltIn));
}

// stdlib §5 / §7.5: the read-only polymorphic head is refused the same way.
BEZEL_TEST(null_cond_linq_collection_iface_8445,
           irocol_sum_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var rc: IReadOnlyCollection<int>? := a; "
             "  Console.WriteLine(rc?.Sum() ?? -1); "),
        kNoValueOnBuiltIn));
}

// stdlib §4.2 / §7.5: the owning polymorphic head is refused the same way.
BEZEL_TEST(null_cond_linq_collection_iface_8445,
           iowningcoll_sum_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        owningProg("  var oc: IOwningCollection<R>? := a; "
                   "  Console.WriteLine(oc?.Sum() ?? -1); "),
        kNoValueOnBuiltIn));
}

// ---------------------------------------------------------------------------
// THE WHOLE stdlib §5 OPERATOR SURFACE — not just `Sum`.
// ---------------------------------------------------------------------------

// stdlib §5 / §7.5: `?.Where(...)` on a polymorphic collection interface is
// refused like the `.` spelling (the operator needs the concrete backing).
BEZEL_TEST(null_cond_linq_collection_iface_8445,
           icol_where_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var c: ICollection<int>? := a; "
             "  Console.WriteLine(c?.Where(lambda(x) => x > 1) ?? -1); "),
        kNoValueOnBuiltIn));
}

// stdlib §5 / §7.5: `?.First()` — an element-access operator — likewise.
BEZEL_TEST(null_cond_linq_collection_iface_8445,
           icol_first_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var c: ICollection<int>? := a; "
             "  Console.WriteLine(c?.First() ?? -1); "),
        kNoValueOnBuiltIn));
}

// stdlib §5 / §7.5: `?.ToArray()` — a materialization operator — likewise.
BEZEL_TEST(null_cond_linq_collection_iface_8445,
           icol_toarray_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var c: ICollection<int>? := a; "
             "  var r := c?.ToArray(); "
             "  Console.WriteLine(r.Count); "),
        kNoValueOnBuiltIn));
}

// stdlib §5 / §7.5: `?.Any()` — a quantifier — likewise.
BEZEL_TEST(null_cond_linq_collection_iface_8445,
           irocol_any_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var rc: IReadOnlyCollection<int>? := a; "
             "  Console.WriteLine(rc?.Any() ?? false); "),
        kNoValueOnBuiltIn));
}

// ---------------------------------------------------------------------------
// THE TWO SPELLINGS AGREE — the refusal is the SAME diagnostic, not merely
// "some Error" on each side.
// ---------------------------------------------------------------------------

// §7.5: "`x?.M` resolves `M` as `x.M` does". The `?.` spelling of `Sum` on an
// `ICollection<int>` must be refused with the `.` spelling's OWN diagnostic,
// byte for byte.
BEZEL_TEST(null_cond_linq_collection_iface_8445,
           icol_sum_same_diagnostic_as_dot_spec_7_5) {
    const std::string dot = firstErrorMessage(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var c: ICollection<int> := a; "
             "  var r := c.Sum(); "
             "  Console.WriteLine(r); "));
    const std::string q = firstErrorMessage(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var c: ICollection<int>? := a; "
             "  var r := c?.Sum(); "
             "  Console.WriteLine(r); "));
    BEZEL_EXPECT_FALSE(dot.empty());
    BEZEL_EXPECT_FALSE(q.empty());
    BEZEL_EXPECT_EQ(q, dot);
}

// The same, on the read-only polymorphic head.
BEZEL_TEST(null_cond_linq_collection_iface_8445,
           irocol_sum_same_diagnostic_as_dot_spec_7_5) {
    const std::string dot = firstErrorMessage(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var c: IReadOnlyCollection<int> := a; "
             "  var r := c.Sum(); "
             "  Console.WriteLine(r); "));
    const std::string q = firstErrorMessage(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var c: IReadOnlyCollection<int>? := a; "
             "  var r := c?.Sum(); "
             "  Console.WriteLine(r); "));
    BEZEL_EXPECT_FALSE(dot.empty());
    BEZEL_EXPECT_FALSE(q.empty());
    BEZEL_EXPECT_EQ(q, dot);
}

// ---------------------------------------------------------------------------
// NO REGRESSION — the heads that DO lower keep lowering, and the #7557 / #853
// fences keep their own diagnostics.
// ---------------------------------------------------------------------------

// stdlib §5 / §4.1: the array-backed LIST interfaces are NOT polymorphic —
// the concrete backing is erased nowhere — so `?.Sum()` on them PERFORMS and
// answers the sum. (This is #7544's fix; the #8445 arm must not swallow it.)
BEZEL_TEST(null_cond_linq_collection_iface_8445,
           ilist_sum_no_regression_spec_7_5) {
    expectStdout(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var il: IList<int>? := a; "
             "  var rol: IReadOnlyList<int>? := a; "
             "  var e: IEnumerable<int>? := a; "
             "  Console.WriteLine(il?.Sum() ?? -1); "
             "  Console.WriteLine(rol?.Sum() ?? -1); "
             "  Console.WriteLine(e?.Sum() ?? -1); "),
        "24\n24\n24\n");
}

// §7.5 Evaluation: a null receiver yields null — the access does not occur.
// The list interfaces keep that behaviour under the `?.` spelling.
BEZEL_TEST(null_cond_linq_collection_iface_8445,
           ilist_sum_null_receiver_spec_7_5) {
    expectStdout(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var nl: IList<int>? := null; "
             "  Console.WriteLine(nl?.Sum() ?? -1); "),
        "-1\n");
}

// stdlib §4.1 / §7.5: `?.Count` and `?.Contains(x)` on the polymorphic heads
// are #7557's population and keep the #853 fence's OWN diagnostic (not the
// #2457 backstop's): the fence is name-based and runs before the backstop.
BEZEL_TEST(null_cond_linq_collection_iface_8445,
           icol_count_contains_keep_853_fence_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var c: ICollection<int>? := a; "
             "  Console.WriteLine(c?.Count ?? -1); "),
        "`.Count` through a polymorphic collection interface"));
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var c: ICollection<int>? := a; "
             "  Console.WriteLine(c?.Contains(8) ?? false); "),
        "`.Contains` through a polymorphic collection interface"));
}

// stdlib §4.1 / §7.5: `?.Contains(x)` on the READ-ONLY polymorphic head is
// the §5 `IEnumerable<T>` operator (the head declares no `Contains`), and the
// `.` spelling of it is refused by the #853 fence — the `?.` spelling must
// reach the same refusal.
//
// The `?.` spelling's RESULT is always nullable (§7.5), so the program must
// consume it in a nullable-friendly way (`?? false`); a program that passed
// the nullable `bool` straight to `Console.WriteLine` would be rejected by
// SEMA ("the argument to `Console.WriteLine` is nullable but the target type
// is non-nullable") before codegen ever ran — that is §7.5's own rule, not a
// disagreement between the spellings.
BEZEL_TEST(null_cond_linq_collection_iface_8445,
           irocol_contains_same_diagnostic_as_dot_spec_7_5) {
    const std::string dot = firstErrorMessage(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var c: IReadOnlyCollection<int> := a; "
             "  Console.WriteLine(c.Contains(8)); "));
    const std::string q = firstErrorMessage(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var c: IReadOnlyCollection<int>? := a; "
             "  Console.WriteLine(c?.Contains(8) ?? false); "));
    BEZEL_EXPECT_FALSE(dot.empty());
    BEZEL_EXPECT_FALSE(q.empty());
    BEZEL_EXPECT_EQ(q, dot);
}
