// Spec-derived regression test for issue #7098 — a method group over a
// builtin collection member SILENTLY non-lowered on a signature mismatch:
// the program compiled, the local degraded to a zero value, and the call
// then SEGFAULTED at 0x8.
//
// THE SPEC.
//
//   spec §27.6 (v0.350) "Method references": "The reference's signature must
//   be compatible with the target function type: its parameter and return
//   types unify with the target's under the standard implicit-conversion
//   rules (§5.1)."
//
//   The DIRECTION is what settles it: at a parameter slot the value ARRIVES
//   as the TARGET's type and is handed to the CALLEE, and spec §5.1 (v0.350)
//   admits no `long` -> `int` conversion - `int` -> `long` is the implicit
//   one, not the other way round.
//
//   stdlib §4.1 (v0.350) declares `Contains(item: T) -> bool` on the
//   borrowing collection interfaces, so over an `IList<int>` the member
//   reference forms `(int) -> bool`.
//
// THE DEFECT.
//
// The #7014 arm (a method group over a builtin collection interface member)
// returned `nullptr` on a signature mismatch - a SILENT non-lowering, not a
// diagnostic.  The local degraded to a zero-initialised value (the #2434
// warning) and the call then read address 0x8:
//
//   var f: (long) -> bool := il.Contains;
//     -> the #2434 degradation warning, then at run time
//        AddressSanitizer: SEGV on unknown address 0x000000000008
//
// A program that COMPILES and then SEGFAULTS is worse than one that is
// rejected, and the asymmetry with the sibling arm (which already refused
// with a clean diagnostic) was itself the defect.
//
// THE ORACLE IS POSITIVE.  The refusal rows assert the refusal, a REAL
// source location, and the POSITIVE diagnostic text; the control row
// asserts the VALUE the formed method group returns.
//
// A conforming formation is still admitted and still CALLABLE - the change
// is a refusal, not a fence.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return "module M; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The §27.6 refusal: the program did NOT run, the rejection is a located
// diagnostic at a REAL source location, and the diagnostic carries the
// §27.6 text.
void expectRefused(const JitResult& r, const char* needle) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§27.6"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
}

}  // namespace

// ===========================================================================
// THE ISSUE'S OWN REPRO — an `IList<int>` member reference formed at an
// incompatible target function type.
//
// `Contains` is declared `Contains(item: T) -> bool` (stdlib §4.1), so over
// an `IList<int>` it forms `(int) -> bool`; the target `(long) -> bool` is
// not reachable from it, and §5.1 admits no `long` -> `int` conversion.
// ===========================================================================
BEZEL_TEST(collection_method_group_signature_7098,
           ilist_method_group_at_an_incompatible_target_is_refused) {
    auto r = runAot(prog(
        "    var a: Array<int> := [1, 2, 3]; "
        "    var il: IList<int> := a; "
        "    var f: (long) -> bool := il.Contains; "));
    expectRefused(r, "the method group over `Contains` has the signature");
}

// ===========================================================================
// THE OTHER RECEIVER — the same class of member reference over the concrete
// `Array<T>` head.
//
// It was already a loud refusal (the #2470 / #2492 fence, which names the
// member and says to call it directly), and it stays one: the point of the
// row is that NOTHING here degrades silently to a zero value.
// ===========================================================================
BEZEL_TEST(collection_method_group_signature_7098,
           array_method_group_is_refused_not_silently_lowered) {
    auto r = runAot(prog(
        "    var a: Array<int> := [1, 2, 3]; "
        "    var f: (long) -> bool := a.Contains; "));
    expectRefused(r, "a bare reference to `Contains`");
}

// ===========================================================================
// THE CONTROL — a formation at the member's OWN signature is still admitted
// and still callable, and it answers the right value.
//
// spec §4.1: `Contains(item: T) -> bool` - 2 IS in the array.
// ===========================================================================
BEZEL_TEST(collection_method_group_signature_7098,
           ilist_method_group_at_its_own_signature_still_forms_and_answers) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var a: Array<int> := [1, 2, 3]; "
        "    var il: IList<int> := a; "
        "    var g: (int) -> bool := il.Contains; "
        "    Console.WriteLine(g(2).ToString()); ")),
        std::string("True"));
}
