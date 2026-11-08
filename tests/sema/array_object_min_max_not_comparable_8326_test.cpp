// Regression tests for issue #8326 — the stdlib §5.1 no-selector
// `Min()` / `Max()` constraint `where T: IComparable<T>` was NOT enforced for
// an `Object` element, so the call was ADMITTED by sema and codegen then
// emitted MALFORMED IR.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   * stdlib §5.1 (v0.355):
//       public extension IEnumerable<T> where T: IComparable<T>
//         public method Min(): T;              // InvalidOperationException if empty
//         public method Max(): T;              // InvalidOperationException if empty
//         public method MinOrNull(): T?;       // null if empty
//         public method MaxOrNull(): T?;       // null if empty
//       end;
//     ⇒ the no-selector form exists ONLY where the element type satisfies
//     `T: IComparable<T>`.
//
//   * spec §8.1 (v0.355) — the declaration of `Object`:
//       public class Object
//         public method GetType(): Type;
//         public virtual method Equals(other: Object?): bool;
//         public virtual method GetHashCode(): int;
//         public virtual method ToString(): string;
//         public static method ReferenceEquals(a: Object?, b: Object?): bool;
//       end Object;
//     It lists NO interface in its inheritance clause and declares NO
//     `CompareTo`.
//
//   * spec §14.2 (v0.355) *Interface conformance* — "For an interface I and a
//     candidate type T, T conforms to I exactly when one of the following
//     holds": rule 1 (a class that LISTS I in its inheritance clause — `Object`
//     lists none) and rule 5 (a primitive conforms to the interfaces its
//     standard-library declaration lists — `Object`'s lists none). So
//     `Object` does NOT conform to `IComparable<Object>`.
//
//   * stdlib §2.8 (v0.355): "The stdlib's primitives and `string` conform to
//     `IComparable<T>`/`IEquatable<T>` at their own type only … both
//     interfaces are invariant … so no `IComparable<Object>`/`IEquatable<Object>`
//     view of them exists" — there is no `IComparable<Object>` view to reach
//     `Object` through either.
//
// ⇒ THE CALL IS NOT A CALL: `Array<Object>.Min()` violates §5.1's constraint,
// so it must be REFUSED — and refused by SEMA, at the call, with the §5.1
// constraint diagnostic.
//
// THE DEFECT (MEASURED on the base, 6b3057bc).
//
//   * `Array<Object>.Min()` was ADMITTED by sema (the §5.1 gate's element
//     filter did not decide `Object`), so the call was typed and lowered;
//   * CODEGEN then took its numeric path and built
//         %18 = icmp slt ptr %15, i32 %17
//     — "Both operands to ICmp instruction are not of the same type!" and
//     "bezelc: codegen error: IR verification failed (malformed IR emitted)".
//     The element is a REFERENCE (a boxed value or a class instance), so the
//     numeric path could never have been right for it.
//
// The same shape over the LAZY receiver (`Where(…)`) and over the
// VTABLE-BACKED receivers (`IEnumerable<T>` / `IList<T>`) aborted the same
// way, so the fix has to cover all three receiver kinds.
//
// ORACLE. `runAot` (the AOT path — the strict oracle; the in-process JIT
// harness is deprecated). Every REFUSING row asserts the SPECIFIC diagnostic
// text, that NO "IR verification failed" text is present (the defect's own
// signature), and that the refusal is a COMPILE-TIME refusal at a REAL
// location — so a program that crashed, hung or was rejected for an unrelated
// reason cannot satisfy a row. Every ACCEPTING row asserts the EXACT STDOUT —
// a VALUE oracle, never "no diagnostic was produced".
//
// The CONTROLS are the positive twins: a numeric element still prints its
// minimum; a `string` element still prints its minimum; a built-in value
// record element still prints its minimum; the SELECTOR overload (whose
// constraint is on `U`, not on `T`) is still admitted over an `Object`
// element; a user's `extension Object { Min() }` still answers an
// `Array<Object>` receiver (the §26.5.1 *Applicability* rule of issue #8199:
// the §5 candidate is not applicable, so the user's extension is); and the
// pre-existing `bool` and `Object?` refusals keep their own wording.

#include <regex>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The program's stdout, verbatim. "<did-not-run>" when the program was
// rejected, crashed or timed out.
std::string out(const std::string& program) {
    JitResult r = runAot(program);
    if (!r.ran) return std::string("<did-not-run>");
    return r.stdoutText;
}

// The §5.1 constraint diagnostic the no-selector `Min()` / `Max()` gate
// reports (stdlib §5.1 / §14.2 / #1198 — the #2770 backstop's family).
const char* kConstraintNeedle =
    "without a selector requires the element type to implement IComparable<T>";

// The refusal must NAME the offending element type, so a row cannot be
// satisfied by a refusal about some other element kind.
const char* kObjectNeedle = "`Object` does not implement IComparable<T>";

// The defect's own signature: the malformed IR the base emitted.
const char* kIrFailureNeedle = "IR verification failed";

// The line the diagnostic carrying `sub` is reported at (0 when there is
// none). `Driver.cpp` prints each diagnostic as `<path>:<line>:<col>:
// <message>`, and that TEXT is what the AOT harness packs into one
// Diagnostic — so the real location is read back off the message.
int errorLineOf(const std::string& program, const std::string& sub) {
    JitResult r = runAot(program);
    static const std::regex kLoc(R"(:(\d+):(\d+): )");
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::size_t pos = d.message.find(sub);
        if (pos == std::string::npos) continue;
        std::size_t lineStart = d.message.rfind('\n', pos);
        lineStart = (lineStart == std::string::npos) ? 0 : lineStart + 1;
        std::size_t lineEnd = d.message.find('\n', pos);
        std::string line = d.message.substr(
            lineStart, (lineEnd == std::string::npos ? d.message.size()
                                                      : lineEnd) -
                           lineStart);
        std::smatch m;
        if (!std::regex_search(line, m, kLoc)) return 0;
        return std::stoi(m[1].str());
    }
    return 0;
}

// The issue's own repro, verbatim in shape. Line 4 carries the CALL
// (`a.Min()`); the element is `Object`.
const char* kObjectArrayMin =
    "module M;\n"
    "class C public static method Main(): int\n"
    "using var a := new Array<Object>();\n"
    "Console.WriteLine(a.Min());\n"
    "return 0; end Main; end C;\n";

}  // namespace

// ---------------------------------------------------------------------------
// ROW 1 — THE ISSUE'S OWN REPRO: `Array<Object>.Min()`. The element type
// `Object` does not satisfy §5.1's `where T: IComparable<T>`, so the call is
// REFUSED — by SEMA, at the call, with the §5.1 constraint diagnostic.
//
// Pre-fix: the program COMPILED and codegen aborted with malformed IR
// ("Both operands to ICmp instruction are not of the same type!" /
// "IR verification failed").
// ---------------------------------------------------------------------------
BEZEL_TEST(array_object_min_max_not_comparable_8326,
           object_element_is_refused_by_sema_not_by_malformed_ir) {
    JitResult r = runAot(kObjectArrayMin);
    BEZEL_EXPECT_FALSE(r.ran);
    // The §5.1 constraint diagnostic, naming the element.
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, kConstraintNeedle));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kObjectNeedle));
    // NOT the defect's own signature: the malformed IR is gone.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kIrFailureNeedle));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "Both operands to ICmp"));
    // A compile-time refusal at a REAL location (not a crash / hang).
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // AT THE CALL: line 4 carries `a.Min()`.
    BEZEL_EXPECT_EQ(errorLineOf(kObjectArrayMin, kConstraintNeedle), 4);
}

// ---------------------------------------------------------------------------
// ROW 2 — the `Max()` sibling (stdlib §5.1 declares the SAME constraint on
// it).
// ---------------------------------------------------------------------------
BEZEL_TEST(array_object_min_max_not_comparable_8326,
           object_element_max_is_refused_by_sema) {
    const std::string program =
        "module M;\n"
        "class C public static method Main(): int\n"
        "using var a := new Array<Object>();\n"
        "Console.WriteLine(a.Max());\n"
        "return 0; end Main; end C;\n";
    JitResult r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kConstraintNeedle));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kObjectNeedle));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kIrFailureNeedle));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_EQ(errorLineOf(program, kConstraintNeedle), 4);
}

// ---------------------------------------------------------------------------
// ROW 3 — the LAZY receiver: `Array<Object>.Where(…)` is an
// `IEnumerable<Object>`, and its `Min()` is the same §5.1 no-selector form.
//
// Pre-fix: the same malformed IR ("icmp slt ptr, i32").
// ---------------------------------------------------------------------------
BEZEL_TEST(array_object_min_max_not_comparable_8326,
           lazy_receiver_over_an_object_element_is_refused_by_sema) {
    const std::string program =
        "module M;\n"
        "class C public static method Main(): int\n"
        "using var a := new Array<Object>();\n"
        "Console.WriteLine(a.Where(lambda(x) => true).Min());\n"
        "return 0; end Main; end C;\n";
    JitResult r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kConstraintNeedle));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kObjectNeedle));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kIrFailureNeedle));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ---------------------------------------------------------------------------
// ROW 4 — the VTABLE-BACKED receiver: an `IEnumerable<Object>` parameter's
// `Min()` dispatches through the interface, not through a concrete array.
//
// Pre-fix: the same malformed IR.
// ---------------------------------------------------------------------------
BEZEL_TEST(array_object_min_max_not_comparable_8326,
           vtable_receiver_over_an_object_element_is_refused_by_sema) {
    const std::string program =
        "module M;\n"
        "import System.Collections.Generic;\n"
        "public class C\n"
        "  public static method F(xs: IEnumerable<Object>): Object => xs.Min();\n"
        "  public static method Main(): int\n"
        "  return 0;\n"
        "  end Main;\n"
        "end C;\n";
    JitResult r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kConstraintNeedle));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kObjectNeedle));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kIrFailureNeedle));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ---------------------------------------------------------------------------
// ROW 5 — the `IList<T>` twin of row 4.
//
// Pre-fix: the same malformed IR.
// ---------------------------------------------------------------------------
BEZEL_TEST(array_object_min_max_not_comparable_8326,
           ilist_receiver_over_an_object_element_is_refused_by_sema) {
    const std::string program =
        "module M;\n"
        "import System.Collections.Generic;\n"
        "public class C\n"
        "  public static method F(xs: IList<Object>): Object => xs.Min();\n"
        "  public static method Main(): int\n"
        "  return 0;\n"
        "  end Main;\n"
        "end C;\n";
    JitResult r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kConstraintNeedle));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kObjectNeedle));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kIrFailureNeedle));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ---------------------------------------------------------------------------
// ROW 6 — CONTROL: a NUMERIC element. `int` conforms to `IComparable<int>`
// (stdlib §9.1 / §2.8), so the §5 `Min()` applies and still prints `1`.
// ---------------------------------------------------------------------------
BEZEL_TEST(array_object_min_max_not_comparable_8326,
           control_int_element_still_prints_its_minimum) {
    BEZEL_EXPECT_EQ(
        out("module M;\n"
            "class C public static method Main(): int\n"
            "using var a := new Array<int>();\n"
            "a.Add(3); a.Add(1);\n"
            "Console.WriteLine(a.Min());\n"
            "return 0; end Main; end C;\n"),
        "1\n");
}

// ---------------------------------------------------------------------------
// ROW 7 — CONTROL: a `string` element (stdlib §9.6 declares
// `String: IComparable<string>`), still admitted, still prints `a`.
// ---------------------------------------------------------------------------
BEZEL_TEST(array_object_min_max_not_comparable_8326,
           control_string_element_still_prints_its_minimum) {
    BEZEL_EXPECT_EQ(
        out("module M;\n"
            "class C public static method Main(): int\n"
            "using var a := new Array<string>();\n"
            "a.Add(\"b\"); a.Add(\"a\");\n"
            "Console.WriteLine(a.Min());\n"
            "return 0; end Main; end C;\n"),
        "a\n");
}

// ---------------------------------------------------------------------------
// ROW 8 — CONTROL: a BUILT-IN VALUE RECORD element (`DateTime`, which
// stdlib §2.5 declares `IComparable<DateTime>` on), still admitted, still
// prints its minimum's ticks.
// ---------------------------------------------------------------------------
BEZEL_TEST(array_object_min_max_not_comparable_8326,
           control_builtin_value_record_element_still_prints_its_minimum) {
    BEZEL_EXPECT_EQ(
        out("module M;\n"
            "class C public static method Main(): int\n"
            "using var a := new Array<DateTime>();\n"
            "a.Add(DateTime.FromTicks(300)); a.Add(DateTime.FromTicks(100));\n"
            "Console.WriteLine(a.Min().Ticks);\n"
            "return 0; end Main; end C;\n"),
        "100\n");
}

// ---------------------------------------------------------------------------
// ROW 9 — CONTROL: the SELECTOR overload. stdlib §5.1 declares
// `public method Min<U>(selector: (T) -> U): U where U: IComparable<U>;` —
// the constraint is on `U`, NOT on the element type `T` — so the §5.1 gate
// does not apply to it, and an `Object` element is ADMITTED.
//
// The gate's arms all read `ce.args.empty()`, so the selector form is not
// touched by this fix.
// ---------------------------------------------------------------------------
BEZEL_TEST(array_object_min_max_not_comparable_8326,
           control_the_selector_overload_is_still_admitted) {
    BEZEL_EXPECT_EQ(
        out("module M;\n"
            "class C public static method Main(): int\n"
            "using var a := new Array<Object>();\n"
            "a.Add(42);\n"
            "Console.WriteLine(a.Min(lambda(x) => 1));\n"
            "return 0; end Main; end C;\n"),
        "1\n");
}

// ---------------------------------------------------------------------------
// ROW 10 — CONTROL: a user's `extension Object { Min() }` ANSWERS an
// `Array<Object>` receiver. Spec §26.5.1 *Applicability*: the §5 block's
// `where T: IComparable<T>` is not satisfied at an `Object` element, so the
// §5 candidate is NOT applicable and never enters the ranking — the user's
// extension is the only applicable candidate, and it answers. This is issue
// #8199's rule, and the `Object` element joins the `bool` element in it.
//
// Pre-fix: the §5 candidate WAS applicable (the filter did not decide
// `Object`), it BEAT the user's extension (§26.5 rule 3's "most specific
// type wins" ranks the APPLICABLE candidates), and codegen aborted with
// malformed IR.
// ---------------------------------------------------------------------------
BEZEL_TEST(array_object_min_max_not_comparable_8326,
           control_a_user_object_extension_answers_an_object_receiver) {
    BEZEL_EXPECT_EQ(
        out("module M;\n"
            "extension Object\n"
            "  public method Min(): int => 4242;\n"
            "  public method Max(): int => 4343;\n"
            "end;\n"
            "class C public static method Main(): int\n"
            "using var a := new Array<Object>();\n"
            "Console.WriteLine(a.Min());\n"
            "Console.WriteLine(a.Max());\n"
            "return 0; end Main; end C;\n"),
        "4242\n4343\n");
}

// ---------------------------------------------------------------------------
// ROW 11 — CONTROL: the `bool` element keeps its own refusal and its own
// wording (the element named in the message is `bool`, not `Object`).
// ---------------------------------------------------------------------------
BEZEL_TEST(array_object_min_max_not_comparable_8326,
           control_the_bool_element_keeps_its_own_refusal) {
    const std::string program =
        "module M;\n"
        "class C public static method Main(): int\n"
        "using var a := new Array<bool>();\n"
        "Console.WriteLine(a.Min());\n"
        "return 0; end Main; end C;\n";
    JitResult r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kConstraintNeedle));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "`bool` does not implement IComparable<T>"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kObjectNeedle));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kIrFailureNeedle));
}

// ---------------------------------------------------------------------------
// ROW 12 — CONTROL: the NULLABLE `Object?` element keeps its own refusal
// (stdlib §5.1's constraint implies a NON-NULL `T` — spec §14.2).
// ---------------------------------------------------------------------------
BEZEL_TEST(array_object_min_max_not_comparable_8326,
           control_the_nullable_object_element_keeps_its_own_refusal) {
    const std::string program =
        "module M;\n"
        "class C public static method Main(): int\n"
        "using var a := new Array<Object?>();\n"
        "Console.WriteLine(a.Min());\n"
        "return 0; end Main; end C;\n";
    JitResult r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kConstraintNeedle));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Object?` does not implement IComparable<T>"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kIrFailureNeedle));
}
