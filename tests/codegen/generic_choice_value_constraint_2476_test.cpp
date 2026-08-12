// Codegen/sema — issue #2476: `where T: value` rejected a CHOICE type
// argument at a generic-method CALL SITE, contra language spec §14.2:
// "`where T: value` restricts T to non-nullable value types — records,
// enums, choices ([§35](#sec-choices)), primitives, inline arrays..., and
// the unit type." A false reject — the compiler was stricter than the spec
// permits, blocking a legal generic-programming pattern.
//
// Root cause: TWO independent, duplicate classifiers exist for "is this
// type argument a value type or a reference type" (§14.2's `class`/`value`
// kind constraints):
//   - `Resolver::categorize` (src/sema/TypeResolution_Checks.cpp) — used
//     for an EXPLICIT generic type-argument list on a TYPE (`new
//     Container<Pt>`). Already correctly classified Choice as Value.
//   - A local lambda `refKindOf`, inline inside `ExprTC::typeCall`
//     (src/sema/ExprTypecheck_Calls.cpp, ~line 1880) — used for a generic
//     METHOD's constraint check at the CALL SITE (`checkMethodGenericConstraints`,
//     invoked for BOTH inferred `F(Color.Red)` and explicit `F<Color>(...)`
//     call shapes). Its own comment claims to "mirror Resolver::categorize"
//     but its `switch` over `TypeRef::Kind::Named`'s declaration kind had no
//     `Choice` case, so a choice fell through to the `return 1;` (reference)
//     fallback meant for declaration-less BUILTIN named types (Array,
//     IEnumerable, ...) — misclassifying every choice as a REFERENCE type.
//     `where T: value` then rejected it (`r == 1` → report("value")); the
//     mirror-image soundness hole is that `where T: class` would have
//     WRONGLY ACCEPTED a choice type argument (a Regression guard below
//     pins this is now correctly rejected too).
//
// Fix: add `ast::TypeDeclKind::Choice` to `refKindOf`'s value-type case,
// alongside Record/Enum, matching `Resolver::categorize`.
//
// Fallout, found by testing (not guessed) before landing, per the
// coordinator's explicit "expect fallout" steer citing the #2465 lesson
// ("narrowing a masking block turned a silent bug into an ICE"): admitting
// Choice into `where T: value` makes call shapes that were UNREACHABLE
// before this fix (the constraint check itself blocked them) newly
// reachable:
//   - `.Equals(...)` / `.GetHashCode()` on a bare-T choice receiver: SAFE —
//     #1091's existing genericSubstSemaTy_ redirect
//     (`tryLowerValueSemanticsCall`) already explicitly includes Choice in
//     `concreteValSem` and dispatches through `lowerValueSemMethodOn` /
//     `emitChoiceEquality` / `emitChoiceHashCode`, which were already
//     implemented and tested for the CONCRETE (non-generic) choice
//     receiver case. Verified VALUE-correct below, not just "didn't crash".
//   - `.ToString()` on a bare-T choice receiver: UNSAFE without a companion
//     fix — segfaulted (`ret ptr null`, the same #2469 crash shape), since
//     nothing in `Emitter::lowerCall`'s generic-substitution ToString
//     fallback has a Choice branch (only Primitive and, after #2469,
//     Record/Enum). A companion diagnostic ("declared via Object but not
//     yet implemented … tracked #2479") was added in the SAME commit as
//     this fix, so the false-reject fix never ships without also closing
//     the crash it exposes. Verified below: a clean diagnostic, not a
//     crash.
//   - `.CompareTo(...)` on a bare-T value receiver silently returns `0`
//     regardless of the substitution (confirmed for BOTH a record and a
//     choice substitution) — but this is PRE-EXISTING and unrelated to
//     Choice specifically (a record substitution reproduces it identically,
//     and a record was already admissible by `where T: value` before this
//     fix), so it is NOT this fix's fallout — filed separately as #2480,
//     not exercised here (encoding a known-separate silent-wrong-value bug
//     as expected behavior in this fix's own suite would be wrong).
//   - `.GetType()` on a bare-T choice receiver: SAFE — already covered
//     uniformly, for every receiver type, by the pre-existing "reflection
//     is not yet implemented" gate (§8.2, #487), which sema types
//     unconditionally regardless of constraint kind.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Per tests/README.md: runAot's WIFSIGNALED path also synthesizes an Error
// diagnostic for a runtime crash, so a genuine compile-time rejection must
// be distinguished by asserting on the diagnostic TEXT, never by exit code
// or `!r.ran` alone.
bool hasError(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

const char* kColor =
    "choice Color Red, Green(value: int), Blue; end Color; ";
}  // namespace

// ── The exact #2476 false reject, both call shapes ──────────────────────

// Inferred call-site generic argument (`F(Color.Red)`): before the fix, this
// was REJECTED with "type argument `Color` does not satisfy the constraint
// `where T: value` (§14.2)" — a false reject per §14.2's explicit admission
// list. After the fix: compiles and runs. Uses `.Equals` (not `.ToString`,
// which needs the separate #2479 implementation) to prove the VALUE, not
// just a clean compile.
BEZEL_TEST(generic_choice_value_constraint_2476,
           inferred_choice_substitution_accepted_value_correct) {
    const std::string program =
        std::string("module M; ") + kColor +
        "class C "
        "  public static method F<T>(val: T): bool where T: value "
        "    return val.Equals(val); "
        "  end F; "
        "  public static method Main(): int "
        "    var b := F(Color.Green(5)); "
        "    return (if b then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// Explicit call-site generic argument (`F<Color>(...)`) — the OTHER call
// shape `checkMethodGenericConstraints` gates (both routes share the same
// `refKindOf` lambda this fix corrects).
BEZEL_TEST(generic_choice_value_constraint_2476,
           explicit_choice_substitution_accepted_value_correct) {
    const std::string program =
        std::string("module M; ") + kColor +
        "class C "
        "  public static method F<T>(val: T): int where T: value "
        "    return val.GetHashCode(); "
        "  end F; "
        "  public static method Main(): int "
        "    var h := F<Color>(Color.Red); "
        "    return (if h <> 0 then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// A DIFFERENT case of the same choice, to pin that GetHashCode/Equals
// actually READ the runtime discriminant + payload (not merely "doesn't
// crash") — Red and Green(5) must hash differently and compare unequal.
BEZEL_TEST(generic_choice_value_constraint_2476,
           distinct_cases_distinguished_through_generic_dispatch) {
    const std::string program =
        std::string("module M; ") + kColor +
        "class C "
        "  public static method Eq<T>(a: T, b: T): bool where T: value "
        "    return a.Equals(b); "
        "  end Eq; "
        "  public static method Main(): int "
        "    var same := Eq(Color.Red, Color.Red); "
        "    var diff := Eq(Color.Red, Color.Blue); "
        "    return (if same and not diff then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// ── Fallout this fix's companion diagnostic used to gate — now IMPLEMENTED ──

// `.ToString()` on a bare-T choice receiver: without a companion fix (added
// in the same commit as this one), this SIGSEGV'd exactly like #2469's
// record crash the instant #2476's classification fix alone landed
// (confirmed during this fix's own development). #2476 closed that crash
// with a clean "declared but not yet implemented" diagnostic (tracked
// #2479); #2479 has since been implemented for the auto-hashable
// case `Color` is (see generic_bare_typeparam_tostring_choice_2479_test.cpp
// for the full regression suite, including the diagnostic that remains for
// the non-auto-hashable classification #2479 left undecided) — so
// this call now dispatches to the real §35.7 format instead of erroring.
// Value-correct, not just clean compile: a payload-free case formats as
// `"TypeName.CaseName"`.
BEZEL_TEST(generic_choice_value_constraint_2476,
           tostring_on_choice_substitution_now_implemented_value_correct) {
    const std::string program =
        std::string("module M; ") + kColor +
        "class C "
        "  public static method F<T>(val: T): string where T: value "
        "    return val.ToString(); "
        "  end F; "
        "  public static method Main(): int "
        "    var s := F(Color.Red); "
        "    return (if s = \"Color.Red\" then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// ── Regression guards ─────────────────────────────────────────────────

// The mirror-image soundness hole this fix ALSO closes: before the fix,
// `refKindOf` misclassified Choice as a REFERENCE type, so `where T: class`
// would have WRONGLY ACCEPTED a choice type argument. After the fix, Choice
// correctly stays a non-reference type and `where T: class` rejects it.
BEZEL_TEST(generic_choice_value_constraint_2476,
           class_constraint_still_rejects_choice) {
    const std::string program =
        std::string("module M; ") + kColor +
        "class C "
        "  public static method F<T>(val: T): int where T: class "
        "    return 1; "
        "  end F; "
        "  public static method Main(): int "
        "    return F(Color.Red); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "does not satisfy the constraint `where T: class`"));
}

// The already-admitted members of §14.2's `where T: value` list are
// unaffected: a RECORD substitution still works (this fix touches only the
// Choice branch of `refKindOf`'s switch).
BEZEL_TEST(generic_choice_value_constraint_2476,
           regression_record_substitution_unaffected) {
    const std::string program =
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C "
        "  public static method F<T>(val: T): bool where T: value "
        "    return val.Equals(val); "
        "  end F; "
        "  public static method Main(): int "
        "    return (if F(new Pt(3, 4)) then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// A PRIMITIVE substitution still works (unaffected — `refKindOf`'s
// Primitive branch is untouched by this fix).
BEZEL_TEST(generic_choice_value_constraint_2476,
           regression_primitive_substitution_unaffected) {
    const std::string program =
        "module M; "
        "class C "
        "  public static method F<T>(val: T): bool where T: value "
        "    return val.Equals(val); "
        "  end F; "
        "  public static method Main(): int "
        "    return (if F(7) then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// A genuinely non-conforming REFERENCE type (a class) is still correctly
// rejected by `where T: value` — this fix does not loosen the class-side
// rejection, only correct the choice-side one.
BEZEL_TEST(generic_choice_value_constraint_2476,
           regression_class_still_rejected_by_value_constraint) {
    const std::string program =
        "module M; "
        "class Widget destructor end destructor; end Widget; "
        "class C "
        "  public static method F<T>(val: T): int where T: value "
        "    return 1; "
        "  end F; "
        "  public static method Main(): int "
        "    using var w := new Widget(); "
        "    return F(w); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "does not satisfy the constraint `where T: value`"));
}
