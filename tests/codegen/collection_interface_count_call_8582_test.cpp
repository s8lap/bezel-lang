// fix #8582 (stdlib §5, §4.1–§4.3, §7.5, v0.357): the stdlib §5
// zero-argument `Count()` reached through a POLYMORPHIC collection interface
// must be refused with a diagnostic — never degraded to a silent zero.
//
// THE RULES.
//   stdlib §4.1 (v0.357) `IReadOnlyCollection<out T>: IEnumerable<T>` declares
//   `pure readonly property Count: int`; `ICollection<T>: IReadOnlyCollection<T>`
//   inherits it; stdlib §4.2's `IOwningCollection<T>: IReadOnlyCollection<T>,
//   IDisposable` inherits it the same way. stdlib §5's `extension IEnumerable<T>`
//   declares `public method Count(): int` — the zero-argument count operator —
//   and stdlib §1's "Block-by-name" row says an instance member blocks a
//   same-named extension, so on these three heads `c.Count()` names THAT member
//   (the inherited `Count` property) through the call spelling.
//   stdlib §4.1–§4.3: `ICollection<T>` is implemented by Array<T> (a
//   bz_array_t*), LinkedList<T> (a bz_llist_t*) and Set<T> / Dictionary<K,V>
//   (a bz_dict_t*) — incompatible runtime representations, with the concrete
//   backing erased at the interface.
//   §7.5 (v0.357) "`x?.M` returns null if `x` is null; otherwise it performs
//   the access. The result type is always nullable." The `?.` spelling
//   resolves the member exactly as the `.` spelling does.
//
// THE DEFECT (issue #8582, MEASURED on this base).
//   The PROPERTY read on these three heads is refused loudly by codegen's
//   #853 fence (`isCollectionInterfaceName`, LlvmBackend_Exprs.cpp) — the
//   erased backing makes every single runtime dispatch unsound. The CALL
//   spelling of the SAME member reached no lowering arm and no fence: the call
//   lowered to NOTHING, the enclosing store degraded to a zero-initialised
//   value behind the `internal:` #2434 warning, and the program exited 0 with
//   a WRONG answer — a three-element `ICollection<int>` printed 0. In
//   expression position the whole `Console.WriteLine` call was dropped and
//   the program printed NOTHING at all. The `?.` spelling degraded the same
//   way (a null `int?`).
//
// THE FIX. The call form reaches the same refusal as the property form, in
// BOTH spellings: `tryLowerCollectionInstanceCall`'s #853 fence now also
// refuses the zero-argument `Count()` on the three polymorphic collection
// interfaces, and the `?.` spelling lowers through the `.` spelling's own
// dispatch (§7.5), so the two spellings fail with the SAME diagnostic.
//
// fix #8633 (stdlib §1 "Block-by-name", §4.1 / §4.2, spec §26.5 rule 1,
// §27.1, §27.7, v0.357): the diagnostic these rows pin is now the SEMA
// property-call refusal, which fires BEFORE codegen is reached. The
// committed member on these three heads is the INHERITED `Count` PROPERTY
// (stdlib §4.1: "pure readonly property Count: int" on
// `IReadOnlyCollection<out T>`), stdlib §1's "Block-by-name" row commits the
// lookup to instance members BY NAME — "inherited ones included" — and a
// property of a non-function type is not callable (§27.1, §27.7): the call
// has no candidate. Codegen's #853 fence remains as the backstop for the
// shapes sema does not type, but the message the user sees is the sema one,
// and it names the head and its declaration.
//
// Every row below is load-bearing: each one is RED on the base commit (the
// access was dropped: the program compiled with the #2434 warning, printed
// nothing or a zero, and exited 0) and GREEN with the fix.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// fix #8633 (stdlib §1 "Block-by-name", §4.1 / §4.2, spec §26.5 rule 1,
// §27.1, §27.7, v0.357): the refusal these rows pin is now the SEMA
// property-call diagnostic, NOT codegen's #853 fence.
//
// The committed member on these three heads is the INHERITED `Count`
// PROPERTY — stdlib §4.1 declares "pure readonly property Count: int" on
// `IReadOnlyCollection<out T>`, and `ICollection<T>` / `IOwningCollection<T>`
// extend it. stdlib §1's "Block-by-name" row commits the lookup to instance
// members BY NAME — "inherited ones included" — so `c.Count()` names THAT
// member, and a property of a non-function type is not callable (§27.1,
// §27.7): the call has no candidate.
//
// The diagnostic therefore names the head and its declaration, and CANNOT be
// satisfied by the old fence's wording ("`Count()` through a polymorphic
// collection interface") — the needle below is built per head so a row cannot
// pass on the WRONG head's diagnostic. Codegen's #853 fence stays as the
// backstop for the shapes sema does not type, but the message the user sees
// is this one.
//
// NOTE: the three polymorphic heads do NOT get the remedy clause ("read it
// without parentheses, `.Count`") — their property READ is itself refused by
// the #853 fence (the concrete backing is erased at the interface), so
// pointing the reader at `.Count` would send them to a refused spelling.
std::string propertyCallFence(const std::string& head,
                              const std::string& section) {
    return "`Count` is a property of `" + head + "`, not a method (stdlib \xC2\xA7" +
           section + ": `Count: int`), so this call has no candidate";
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
// collection interfaces. `Box` is a class element, so the owning collection
// disposes it at scope end.
std::string owningProg(const std::string& body) {
    return "module M; import System; import System.Linq; "
           "class Box public v: int := 0; end Box; "
           "class P "
           "  public static method Main(): int "
           "    using var a := new OwningArray<Box>(); "
           "    a.Add(new Box()); a.Add(new Box()); " +
           body + " return 0; end Main; end P;";
}

}  // namespace

// ---------------------------------------------------------------------------
// THE ISSUE'S OWN REPRO — the `.` spelling of `Count()` on the polymorphic
// collection interfaces.
// ---------------------------------------------------------------------------

// stdlib §5 / §4.1: `ICollection<int>.Count()` is REFUSED. Before the fix
// the call lowered to nothing: the local `n` was degraded to 0 behind the
// `internal:` #2434 warning and the program printed 0 for a three-element
// collection.
BEZEL_TEST(collection_interface_count_call_8582,
           icol_count_call_rejected_spec_5_stdlib_4_1) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var c: ICollection<int> := a; "
             "  var n := c.Count(); "
             "  Console.WriteLine(n); "),
        propertyCallFence("ICollection", "4.1")));
}

// stdlib §5 / §4.1: the read-only polymorphic head is refused the same way.
BEZEL_TEST(collection_interface_count_call_8582,
           irocol_count_call_rejected_spec_5_stdlib_4_1) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var rc: IReadOnlyCollection<int> := a; "
             "  var n := rc.Count(); "
             "  Console.WriteLine(n); "),
        propertyCallFence("IReadOnlyCollection", "4.1")));
}

// stdlib §5 / §4.2: the owning polymorphic head is refused the same way.
BEZEL_TEST(collection_interface_count_call_8582,
           iowningcoll_count_call_rejected_spec_5_stdlib_4_2) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        owningProg("  var oc: IOwningCollection<Box> := a; "
                   "  var n := oc.Count(); "
                   "  Console.WriteLine(n); "),
        propertyCallFence("IOwningCollection", "4.2")));
}

// ---------------------------------------------------------------------------
// THE SAME MEMBER IN EXPRESSION POSITION — before the fix the whole
// `Console.WriteLine` call was dropped and the program printed NOTHING.
// ---------------------------------------------------------------------------

BEZEL_TEST(collection_interface_count_call_8582,
           icol_count_call_expression_position_rejected_spec_5_stdlib_4_1) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var c: ICollection<int> := a; "
             "  Console.WriteLine(c.Count()); "),
        propertyCallFence("ICollection", "4.1")));
}

// ---------------------------------------------------------------------------
// THE `?.` SPELLING — §7.5 resolves the member exactly as the `.` spelling
// does, so it must be refused with the SAME diagnostic (before the fix it
// degraded to a null `int?` behind the #2434 warning).
// ---------------------------------------------------------------------------

BEZEL_TEST(collection_interface_count_call_8582,
           icol_count_call_null_cond_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var c: ICollection<int>? := a; "
             "  var n := c?.Count(); "
             "  Console.WriteLine(n ?? -1); "),
        propertyCallFence("ICollection", "4.1")));
}

BEZEL_TEST(collection_interface_count_call_8582,
           irocol_count_call_null_cond_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        prog("  var a: Array<int> := [7, 8, 9]; "
             "  var rc: IReadOnlyCollection<int>? := a; "
             "  var n := rc?.Count(); "
             "  Console.WriteLine(n ?? -1); "),
        propertyCallFence("IReadOnlyCollection", "4.1")));
}

BEZEL_TEST(collection_interface_count_call_8582,
           iowningcoll_count_call_null_cond_rejected_spec_7_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        owningProg("  var oc: IOwningCollection<Box>? := a; "
                   "  var n := oc?.Count(); "
                   "  Console.WriteLine(n ?? -1); "),
        propertyCallFence("IOwningCollection", "4.2")));
}
