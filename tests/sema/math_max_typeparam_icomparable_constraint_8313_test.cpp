// Regression tests for issue #8313 — the `where T: IComparable<T>` constraint
// of stdlib §2.2's `Math.Min<T>` / `Math.Max<T>` / `Math.Clamp<T>` was NOT
// enforced AT THE CALL, so an unsatisfied instantiation was admitted by sema
// and only failed (or failed BADLY) later, in codegen.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   * language spec §14.2 (v0.355) *A type parameter as the type argument*:
//       "Where a constraint's satisfaction turns on a type parameter in
//        scope, it is decided at the instantiation site (§14.1) that names
//        the argument, by the constraints of the parameters it turns on …
//        An unsatisfied constraint is a compile error at that site, naming
//        the constraint, the argument and the parameter it turns on; nothing
//        of it is carried to the naming declaration's instantiations, which
//        satisfy the constraint by construction (§14.1 *Instantiation and its
//        site*)."
//     and its case 1:
//       "The argument is a type parameter `P` … a call `G<U>(…)` at written or
//        inferred type arguments in the body of `F<U>`: the constraint is
//        decided at that site, by `P`'s own constraints. `P` satisfies the
//        constraint exactly when its constraints guarantee it at every
//        substitution they admit, as a matter of language rule."
//     and *Interface conformance* rule 6:
//       "T is a type parameter whose constraints guarantee conformance to I:
//        a type-bound constraint naming I or an interface extending I, or a
//        class bound naming a class that conforms to I under rule 1, directly
//        or through a `where T: U` chain."
//
//   * stdlib §2.2 (v0.355):
//       public static method Min<T>(a: T, b: T): T where T: IComparable<T>;
//       public static method Max<T>(a: T, b: T): T where T: IComparable<T>;
//       public static method Clamp<T>(value: T, low: T, high: T): T
//         where T: IComparable<T>;
//
// ⇒ THE DECISION IS AT THE CALL SITE — the `Math.Max(a, b)` in the generic
// BODY — and the diagnostic names the constraint, the argument (the type
// parameter) and the parameter it turns on.
//
// THE DEFECT (MEASURED on the base, 6b3057bc).
//
//   * `G.F(3, 4)` over `F<T>(a: T, b: T): T` (NO constraint) COMPILED and
//     PRINTED `4` (exit 0) — the constraint was never read;
//   * the CLASS-TYPED instantiation of that same body (`G.F(x, y)` with
//     `x, y: Foo`) was refused LATE, in CODEGEN, with the MISATTRIBUTED
//     #2786 message ("`Math.Max` could not lower a comparison for this
//     argument type … (#2786)") pointing INTO the generic body's line.
//
// ORACLE. `runAot` (the AOT path — the strict oracle; the in-process JIT
// harness is deprecated). Every ACCEPTING row asserts the EXACT STDOUT — a
// VALUE oracle, never "no diagnostic was produced" — and every REFUSING row
// asserts the SPECIFIC diagnostic text AND the line it is reported at, so a
// program rejected for an unrelated reason cannot satisfy a row.
//
// The CONTROLS are the positive twins: the CONSTRAINED form
// (`where T: IComparable<T>`) is ADMITTED and still prints `4`; the concrete
// `Math.Max(3, 4)` is ADMITTED and still prints `4`; a comparable class
// element is ADMITTED; a NON-comparable class element keeps the pre-existing
// §14.2 refusal (the #1875 gate) unchanged.

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

// REJECTION with the SPECIFIC diagnostic text: a program that was rejected
// for an unrelated reason (or that compiled) must NOT satisfy the row.
bool rejectsWith(const std::string& program, const std::string& needle) {
    JitResult r = runAot(program);
    if (r.ran || !r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

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

// The §14.2 constraint diagnostic the stdlib §2.2 gate reports for a type
// argument that does not satisfy `where T: IComparable<T>`.
const char* kConstraintNeedle =
    "does not satisfy the constraint `where T: IComparable<T>`";

// The codegen #2786 guard's message — the ONE the defect produced. A row
// that pins the constraint diagnostic must NOT see this.
const char* kMisattributedCodegenNeedle = "#2786";

// The issue's own repro, verbatim in shape. Line 2 carries the generic
// method's BODY (`Math.Max(a, b)`); line 4 carries the INSTANTIATION
// (`G.F(3, 4)`). The two are different lines so a row can tell WHERE the
// rejection lands.
const char* kUnconstrainedRepro =
    "module M;\n"
    "public static class G public static method F<T>(a: T, b: T): T return "
    "Math.Max(a, b); end F; end G;\n"
    "class C public static method Main(): int\n"
    "Console.WriteLine(G.F(3, 4));\n"
    "return 0; end Main; end C;\n";

// The CONTROL: the SAME body with the constraint stdlib §2.2's `Max<T>`
// declares.
const char* kConstrainedRepro =
    "module M;\n"
    "public static class G public static method F<T>(a: T, b: T): T where T: "
    "IComparable<T> return Math.Max(a, b); end F; end G;\n"
    "class C public static method Main(): int\n"
    "Console.WriteLine(G.F(3, 4));\n"
    "return 0; end Main; end C;\n";

// A generic method whose body calls `Math.Max(a, b)` over its own type
// parameter, with the constraint clause spelled `clause` ("" = none).
std::string maxOverTypeParam(const std::string& clause) {
    return "module M;\n"
           "public static class G public static method F<T>(a: T, b: T): T" +
           clause +
           " return Math.Max(a, b); end F; end G;\n"
           "class C public static method Main(): int\n"
           "Console.WriteLine(G.F(3, 4));\n"
           "return 0; end Main; end C;\n";
}

// A comparable CLASS element (spec §14.2 rule 1).
const char* kComparableClass =
    "public class Foo: IComparable<Foo>\n"
    "  public X: int;\n"
    "  public constructor(x: int)\n"
    "    X := x;\n"
    "  end;\n"
    "  public method CompareTo(other: Foo): int => X - other.X;\n"
    "end Foo;\n";

}  // namespace

// ---------------------------------------------------------------------------
// ROW 1 — THE ISSUE'S OWN REPRO: `Math.Max(a, b)` over an UNCONSTRAINED type
// parameter. The constraint turns on `T`, and `T` guarantees nothing, so the
// call is refused AT THE CALL SITE — line 2, the generic body's line — with
// the constraint diagnostic.
//
// Pre-fix: the program COMPILED and printed `4`.
// ---------------------------------------------------------------------------
BEZEL_TEST(math_max_typeparam_icomparable_constraint_8313,
           unconstrained_type_parameter_is_refused_at_the_call_site) {
    BEZEL_EXPECT_TRUE(rejectsWith(kUnconstrainedRepro, kConstraintNeedle));
    // AT THE CALL SITE: line 2 is the body's line; line 4 is the
    // instantiation's. §14.2 puts the error at the site that names the
    // argument — the `Math.Max(a, b)` — and nowhere else.
    BEZEL_EXPECT_EQ(errorLineOf(kUnconstrainedRepro, kConstraintNeedle), 2);
    // NOT the codegen #2786 message the defect produced.
    BEZEL_EXPECT_FALSE(
        rejectsWith(kUnconstrainedRepro, kMisattributedCodegenNeedle));
}

// ---------------------------------------------------------------------------
// ROW 2 — THE CONTROL: the SAME body with `where T: IComparable<T>`. §14.2
// rule 6: the bound names `IComparable` at the parameter, so `T` DOES
// guarantee conformance to `IComparable<T>` — the call is ADMITTED and the
// program still prints `4`.
//
// GREEN on the base too — the control that shows the shape is genuinely
// admitted rather than refused for an unrelated reason.
// ---------------------------------------------------------------------------
BEZEL_TEST(math_max_typeparam_icomparable_constraint_8313,
           constrained_type_parameter_is_still_admitted) {
    BEZEL_EXPECT_EQ(out(kConstrainedRepro), "4\n");
}

// ---------------------------------------------------------------------------
// ROW 3 — the `Math.Min` sibling (stdlib §2.2 declares the SAME constraint
// on it).
// ---------------------------------------------------------------------------
BEZEL_TEST(math_max_typeparam_icomparable_constraint_8313,
           min_sibling_is_refused_at_the_call_site) {
    const std::string program =
        "module M;\n"
        "public static class G public static method F<T>(a: T, b: T): T "
        "return Math.Min(a, b); end F; end G;\n"
        "class C public static method Main(): int\n"
        "Console.WriteLine(G.F(3, 4));\n"
        "return 0; end Main; end C;\n";
    BEZEL_EXPECT_TRUE(rejectsWith(program, kConstraintNeedle));
    BEZEL_EXPECT_EQ(errorLineOf(program, kConstraintNeedle), 2);
}

// ---------------------------------------------------------------------------
// ROW 4 — the `Math.Clamp` sibling (stdlib §2.2's third member with the
// same constraint).
// ---------------------------------------------------------------------------
BEZEL_TEST(math_max_typeparam_icomparable_constraint_8313,
           clamp_sibling_is_refused_at_the_call_site) {
    const std::string program =
        "module M;\n"
        "public static class G public static method F<T>(a: T, b: T, c: T): T "
        "return Math.Clamp(a, b, c); end F; end G;\n"
        "class C public static method Main(): int\n"
        "Console.WriteLine(G.F(3, 1, 4));\n"
        "return 0; end Main; end C;\n";
    BEZEL_EXPECT_TRUE(rejectsWith(program, kConstraintNeedle));
    BEZEL_EXPECT_EQ(errorLineOf(program, kConstraintNeedle), 2);
}

// ---------------------------------------------------------------------------
// ROW 5 — the EXPLICIT type list: `Math.Max<T>(a, b)`. The list BINDS the
// stdlib's type parameter to the body's own `T`, so the constraint turns on
// the same parameter and the same decision follows. (The method-REFERENCE
// formation already refused this shape — #6295; the CALL now agrees.)
// ---------------------------------------------------------------------------
BEZEL_TEST(math_max_typeparam_icomparable_constraint_8313,
           explicit_type_argument_over_an_unconstrained_parameter_is_refused) {
    const std::string program =
        "module M;\n"
        "public static class G public static method F<T>(a: T, b: T): T "
        "return Math.Max<T>(a, b); end F; end G;\n"
        "class C public static method Main(): int\n"
        "Console.WriteLine(G.F(3, 4));\n"
        "return 0; end Main; end C;\n";
    BEZEL_EXPECT_TRUE(rejectsWith(program, kConstraintNeedle));
    BEZEL_EXPECT_EQ(errorLineOf(program, kConstraintNeedle), 2);
}

// ---------------------------------------------------------------------------
// ROW 6 — the CLASS-TYPED INSTANTIATION of the unconstrained body — the
// issue's second shape. `G.F(x, y)` with `x, y: Foo` (a class that
// implements no `IComparable`).
//
// Pre-fix: refused LATE, in CODEGEN, with the MISATTRIBUTED #2786 message
// pointing INTO the generic body's line. Now: refused at the CALL SITE with
// the constraint diagnostic — and NOT at the instantiation (line 7).
// ---------------------------------------------------------------------------
BEZEL_TEST(math_max_typeparam_icomparable_constraint_8313,
           class_typed_instantiation_is_refused_by_the_constraint_not_by_codegen) {
    const std::string program =
        "module M;\n"
        "public class Foo public X: int; public constructor(x: int) X := x; "
        "end; end Foo;\n"
        "public static class G public static method F<T>(a: T, b: T): T return "
        "Math.Max(a, b); end F; end G;\n"
        "class C public static method Main(): int\n"
        "using var x := new Foo(1);\n"
        "using var y := new Foo(2);\n"
        "Console.WriteLine(G.F(x, y).X);\n"
        "return 0; end Main; end C;\n";
    BEZEL_EXPECT_TRUE(rejectsWith(program, kConstraintNeedle));
    // The CALL SITE (line 3 — the body's line), NOT the instantiation
    // (line 7).
    BEZEL_EXPECT_EQ(errorLineOf(program, kConstraintNeedle), 3);
    // NOT the codegen #2786 message.
    BEZEL_EXPECT_FALSE(
        rejectsWith(program, kMisattributedCodegenNeedle));
    BEZEL_EXPECT_FALSE(
        rejectsWith(program, "could not lower a comparison"));
}

// ---------------------------------------------------------------------------
// ROW 7 — `where T: IComparable<int>` — the INVARIANCE case (spec §12.9:
// `IComparable` is invariant). The bound guarantees `T: IComparable<int>`,
// NOT `T: IComparable<T>`, so §14.2 rule 6 does not hold and the call is
// refused. The spec's own worked example for `where T: IComparable<int>`
// admits "a mix of primitive, class, record, and interface substitutions" —
// it is a DIFFERENT constraint from the one stdlib §2.2 declares.
//
// Pre-fix: the program COMPILED and printed `4`.
// ---------------------------------------------------------------------------
BEZEL_TEST(math_max_typeparam_icomparable_constraint_8313,
           a_bound_naming_another_instantiation_does_not_establish_it) {
    BEZEL_EXPECT_TRUE(rejectsWith(maxOverTypeParam(" where T: IComparable<int>"),
                                   kConstraintNeedle));
}

// ---------------------------------------------------------------------------
// ROW 8 — `where T: class` — a KIND constraint, naming no type bound at
// all. §14.2 rule 6: only "a type-bound constraint naming I or an interface
// extending I, or a class bound naming a class that conforms to I" can
// guarantee conformance to `IComparable<T>`, and an ordinary class's
// conforming set is open — so `T` may be a class that implements no
// `IComparable`.
//
// Pre-fix: the program was refused LATE, in codegen, with the #2786
// message (measured); now it is refused at the call site with the
// constraint diagnostic.
// ---------------------------------------------------------------------------
BEZEL_TEST(math_max_typeparam_icomparable_constraint_8313,
           a_class_bound_does_not_establish_the_comparable_constraint) {
    BEZEL_EXPECT_TRUE(rejectsWith(maxOverTypeParam(" where T: class"),
                                   kConstraintNeedle));
}

// ---------------------------------------------------------------------------
// ROW 9 — `where T: INumber<T>` — stdlib §2.19's `INumber<T>` declares NO
// base interface, so it does not extend `IComparable<T>` (§14.2 rule 6
// requires "naming I or an interface extending I"). The bound therefore does
// not establish stdlib §2.2's constraint.
//
// Pre-fix: the program COMPILED and printed `4`.
// ---------------------------------------------------------------------------
BEZEL_TEST(math_max_typeparam_icomparable_constraint_8313,
           an_inumber_bound_does_not_establish_the_comparable_constraint) {
    BEZEL_EXPECT_TRUE(rejectsWith(maxOverTypeParam(" where T: INumber<T>"),
                                   kConstraintNeedle));
}

// ---------------------------------------------------------------------------
// ROW 10 — CONTROL: the CONSTRAINT ON THE ENCLOSING TYPE (not on the
// method). `class G<T> where T: IComparable<T>` — the same constraint set,
// read from the type's own clause — is ADMITTED and still prints `4`.
// ---------------------------------------------------------------------------
BEZEL_TEST(math_max_typeparam_icomparable_constraint_8313,
           a_type_level_constraint_is_still_admitted) {
    const std::string program =
        "module M;\n"
        "public static class G<T> where T: IComparable<T>\n"
        "  public static method F(a: T, b: T): T => Math.Max(a, b);\n"
        "end G;\n"
        "class C public static method Main(): int\n"
        "Console.WriteLine(G<int>.F(3, 4));\n"
        "return 0; end Main; end C;\n";
    BEZEL_EXPECT_EQ(out(program), "4\n");
}

// ---------------------------------------------------------------------------
// ROW 11 — CONTROL: the CONCRETE call `Math.Max(3, 4)` — no type parameter
// turns on, so nothing is decided here. Still ADMITTED, still prints `4`.
// ---------------------------------------------------------------------------
BEZEL_TEST(math_max_typeparam_icomparable_constraint_8313,
           the_concrete_call_is_unchanged) {
    BEZEL_EXPECT_EQ(
        out("module M;\n"
            "class C public static method Main(): int\n"
            "Console.WriteLine(Math.Max(3, 4));\n"
            "return 0; end Main; end C;\n"),
        "4\n");
}

// ---------------------------------------------------------------------------
// ROW 12 — CONTROL: a COMPARABLE CLASS element — the constraint holds at the
// concrete argument type (§14.2 rule 1), so the call is ADMITTED and the
// result is the class.
// ---------------------------------------------------------------------------
BEZEL_TEST(math_max_typeparam_icomparable_constraint_8313,
           a_comparable_class_argument_is_still_admitted) {
    BEZEL_EXPECT_EQ(
        out("module M;\n" + std::string(kComparableClass) +
            "class C public static method Main(): int\n"
            "using var a := new Foo(1);\n"
            "using var b := new Foo(2);\n"
            "Console.WriteLine(Math.Max(a, b).X);\n"
            "return 0; end Main; end C;\n"),
        "2\n");
}

// ---------------------------------------------------------------------------
// ROW 13 — CONTROL: a NON-COMPARABLE CLASS element — the PRE-EXISTING
// §14.2 refusal (the #1875 gate) is unchanged by this fix: the program is
// still refused, and with the same wording.
// ---------------------------------------------------------------------------
BEZEL_TEST(math_max_typeparam_icomparable_constraint_8313,
           a_non_comparable_class_argument_is_still_refused) {
    const std::string program =
        "module M;\n"
        "public class Foo public X: int; public constructor(x: int) X := x; "
        "end; end Foo;\n"
        "class C public static method Main(): int\n"
        "using var a := new Foo(1);\n"
        "using var b := new Foo(2);\n"
        "Console.WriteLine(Math.Max(a, b).X);\n"
        "return 0; end Main; end C;\n";
    BEZEL_EXPECT_TRUE(rejectsWith(program, "does not satisfy the constraint"));
    BEZEL_EXPECT_FALSE(
        rejectsWith(program, kMisattributedCodegenNeedle));
}
