// Regression tests for issue #8026: the `?.` spelling of the universal
// `Object.GetType` — §8.1 (the universal member), §8.2 (what `GetType()`
// returns), §7.5 (a null-conditional member access resolves as the plain
// member access does), §11.7 / §12.2 (a declared member of the same name is
// NOT the universal one), stdlib §2.13 (the `Type` class), v0.350.
//
// §8.1: `GetType` is a universal `Object` member — present on every type
// WITHOUT a declared member symbol. §8.2: `v.GetType()` returns the `Type`
// value for the runtime type of `v`; the runtime cannot produce that value
// yet (stdlib §2.13's `Type` needs a type-descriptor table that is not
// emitted), so the compiler REFUSES the call — issue #487.
//
// §7.5: "`x?.M` resolves `M` as `x.M` does". So `t?.GetType()` must be
// refused exactly as `t.GetType()` is.
//
// THE DEFECT (#6859's row, `tests/conformance/rules/cases/s8_1_0004.bzl`):
// the `?.` spelling reached no lowering arm at all. The universal member
// carries no MethodSymbol, so `lowerNullCondCall` found no callee, lowered
// NOTHING, and the enclosing store was DEGRADED to a zero value behind an
// `internal:` note. The program COMPILED, exited 0, and printed `False`
// where §8.2 requires `True` — the silent wrong-code shape that
// `scripts/check_internal_residual.py` ratchets, and the +1 that made that
// ratchet RED on this tree.
//
// The rows below pin the refusal: the `?.` spelling is REJECTED with the
// shared diagnostic, the program never runs, and the rejected compile carries
// NO `internal:` note. They FAIL on the pre-fix code, where the program
// compiled and ran.
//
// The control rows pin that the refusal is NARROW: an ordinary `?.` call
// still works (§7.5), a static class's own DECLARED static `GetType` is still
// CALLED (§11.7 / §12.2, #6763), and the `.` spelling keeps sema's refusal.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// The `?.` spelling of the universal `GetType`, on a nullable task — the
// shape of `tests/conformance/rules/cases/s8_1_0004.bzl`.
std::string nullCondGetTypeProgram() {
    return R"BZL(
module Conf;
public class Program
  public static method Main(): int
    using var t0 := Task.FromResult(2);
    var t: Task<int>? := t0;
    var b := t?.GetType()?.IsClass ?? false;
    Console.WriteLine(b.ToString());
    return 0;
  end Main;
end Program;
)BZL";
}

// The same refusal, on a nullable CLASS receiver, without the chain.
std::string nullCondGetTypeOnClassProgram() {
    return R"BZL(
module Conf;
public class Widget
  public constructor()
  end constructor;
end Widget;
public class Program
  public static method Main(): int
    var w0 := new Widget();
    var w: Widget? := w0;
    var x := w?.GetType();
    Console.WriteLine(x.ToString());
    return 0;
  end Main;
end Program;
)BZL";
}

// An error carrying the given text, on the REJECT path.
//
// The AOT harness fills `JitResult::diagnostics` ONLY on a reject, and it
// fills it with ONE synthetic Error whose message is the driver's captured
// stderr — so the compiler's own diagnostic text is IN that message.
bool rejectedWith(const JitResult& r, const std::string& needle) {
    if (!bezel::test::failedAtCompileTimeWithRealLocation(r)) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// The REJECT path's diagnostics carry no `internal:` note: the refusal is a
// real diagnostic, so nothing degraded silently behind it.
//
// `r.ran` is false on this path — the program was rejected, never executed —
// and the harness fills the diagnostics only on a reject, so this reads the
// diagnostics of the very compile that was rejected. The early-out keeps the
// predicate reject-only: a program that RAN compiled cleanly, so it carries
// no reject diagnostics to scan (see scripts/check_vacuous_tests.py's
// Class A shape).
bool rejectPathCarriesInternalNote(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("internal:") != std::string::npos) return true;
    return false;
}

}  // namespace

// §8.1 / §8.2 / §7.5 (#8026, #6859): the `?.` spelling of the universal
// `GetType` is REFUSED, not degraded. Pre-fix this program compiled, ran, and
// printed `False` where §8.2 requires `True`.
BEZEL_TEST(null_cond_gettype_8026, null_conditional_get_type_is_refused_spec_8_2) {
    auto r = runAot(nullCondGetTypeProgram());
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "`GetType()` is not yet implemented in codegen"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_FALSE(rejectPathCarriesInternalNote(r));
}

// §8.1 / §8.2 / §7.5 (#8026): the same refusal on a nullable CLASS receiver,
// with no chain — the refusal is the CALL's, not the chain's.
BEZEL_TEST(null_cond_gettype_8026, null_conditional_get_type_on_class_is_refused_spec_8_2) {
    auto r = runAot(nullCondGetTypeOnClassProgram());
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "`GetType()` is not yet implemented in codegen"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_FALSE(rejectPathCarriesInternalNote(r));
}

// §8.2 (#8026): the `.` spelling keeps sema's refusal — the two spellings
// agree on the VERDICT (both are refused), which is what §7.5 requires.
// Control: passes on the pre-fix code as well.
BEZEL_TEST(null_cond_gettype_8026, dot_spelling_keeps_its_refusal_spec_8_2) {
    auto r = runAot(R"BZL(
module Conf;
public class Program
  public static method Main(): int
    using var t0 := Task.FromResult(2);
    var t: Task<int>? := t0;
    var x := t.GetType();
    Console.WriteLine(x.ToString());
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "reflection is not yet implemented: 'GetType()'"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §7.5 (#8026) control: an ordinary `?.` call is untouched by the refusal —
// it still runs and still prints its value. The refusal is keyed on the
// member NAME, so a `?.` call to a declared member must not be caught by it.
BEZEL_TEST(null_cond_gettype_8026, ordinary_null_conditional_call_still_runs_spec_7_5) {
    BEZEL_EXPECT_EQ(
        runAotOut(R"BZL(
module Conf;
public class Widget
  public constructor()
  end constructor;
  public method Name(): string => "w";
end Widget;
public class Program
  public static method Main(): int
    var w0 := new Widget();
    var w: Widget? := w0;
    Console.WriteLine(w?.Name() ?? "null");
    return 0;
  end Main;
end Program;
)BZL"),
        std::string("w"));
}

// §11.7 / §12.2 (#6763, #8026) control: a static class's own DECLARED
// static `GetType` is a member of that class, not the universal
// `Object.GetType` — it is still CALLED, on both spellings the language has
// for a static member.
BEZEL_TEST(null_cond_gettype_8026, declared_static_get_type_is_still_called_spec_11_7) {
    BEZEL_EXPECT_EQ(
        runAotOut(R"BZL(
module Conf;
public static class V
  public static method GetType(): int => 7;
end V;
public class Program
  public static method Main(): int
    Console.WriteLine(V.GetType());
    return 0;
  end Main;
end Program;
)BZL"),
        std::string("7"));
}
