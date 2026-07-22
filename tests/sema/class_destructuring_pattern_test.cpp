// Spec-derived tests for §19.3: "Destructuring applies to records only".
//
// §19.3 states explicitly:
//   "Destructuring applies to records only — the positional binding is over
//   a record's positional fields (§9.1). Classes do not have a positional-
//   destructuring surface, even when they declare a primary constructor
//   (§22.10): primary-constructor parameters are not auto-promoted to public
//   properties, and the positional shape is not part of the class's pattern-
//   match surface. To match a class hierarchy, use a type-test pattern
//   (`Dog`, `var d: Dog`) and access fields through the bound name."
//
// These tests verify that the compiler emits a compile error for a
// DestructuringPattern `ClassName(...)` applied to a class type in both
// expression-position (case expr) and statement-position (case stmt), and
// that record destructuring and class type-test patterns still work correctly.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some error-severity diagnostic mentions class destructuring.
bool hasClassDestructDiag(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        bool mentionsDestruct =
            d.message.find("destructuring") != std::string::npos ||
            d.message.find("positional") != std::string::npos;
        bool mentionsClass = d.message.find("class") != std::string::npos;
        if (mentionsDestruct && mentionsClass) return true;
    }
    return false;
}

} // namespace

// ── §19.3: class positional-destructuring pattern is a compile error ─────────

// Repro from the bug report: Circle(_) on a class type must be a sema error,
// not a codegen IR crash. §19.3 explicitly forbids class positional patterns.
BEZEL_TEST(sema_class_destruct, class_positional_pattern_in_case_expr_spec_19_3) {
    auto r = parseSemaAll(
        "module M; "
        "class Shape  public virtual method K(): int return 0; end K; end Shape; "
        "class Circle : Shape "
        "  public _r: int; "
        "  public constructor(r: int) _r := r; end constructor; "
        "  public override method K(): int return _r; end K; "
        "end Circle; "
        "class C public static method Main(): int "
        "  using var c := new Circle(5); "
        "  var shape: Shape := c; "
        "  return (case shape of "
        "    Circle(_) => 1, "
        "    _ => 0); "
        "end Main; end C;");
    // §19.3: class positional-destructuring is a compile error.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasClassDestructDiag(r));
}

// A class with multiple sub-patterns is also forbidden per §19.3.
BEZEL_TEST(sema_class_destruct, class_multi_subpat_in_case_expr_spec_19_3) {
    auto r = parseSemaAll(
        "module M; "
        "class Dog "
        "  public name: string; "
        "  public age: int; "
        "  public constructor(n: string, a: int) name := n; age := a; "
        "  end constructor; "
        "end Dog; "
        "class C public static method Main(): int "
        "  using var d := new Dog(\"Rex\", 3); "
        "  var obj: Dog := d; "
        "  return (case obj of "
        "    Dog(_, _) => 1, "
        "    _ => 0); "
        "end Main; end C;");
    // §19.3: Dog(_, _) on a class is a compile error.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasClassDestructDiag(r));
}

// Class positional-destructuring in statement-position case is also an error.
BEZEL_TEST(sema_class_destruct, class_positional_pattern_in_case_stmt_spec_19_3) {
    auto r = parseSemaAll(
        "module M; "
        "class Animal "
        "  public sound: string; "
        "  public constructor(s: string) sound := s; end constructor; "
        "end Animal; "
        "class C public static method Main(): int "
        "  using var a := new Animal(\"meow\"); "
        "  var x: int := 0; "
        "  case a of "
        "    Animal(_) => x := 1 "
        "  end; "
        "  return x; "
        "end Main; end C;");
    // §19.3: Animal(_) on a class is a compile error in stmt-position too.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasClassDestructDiag(r));
}

// ── §19.3 / Appendix C: record-destructuring binding count must match the ────
//    record's positional-field count (R34). An over-/under-bound pattern was
//    accepted, then crashed codegen with malformed IR ("case.end no terminator")
//    in expression position and silently mis-took an arm in statement position.

namespace {
bool hasArityDiag(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("binding count must match") != std::string::npos)
            return true;
    }
    return false;
}
}  // namespace

// R34: over-binding `Pt(x, y, z)` on a 2-field record (case EXPRESSION) — was
// IR_FAIL; now a clean §19.3 arity diagnostic.
BEZEL_TEST(sema_class_destruct, record_destructure_over_binding_expr_r34) {
    auto r = parseSemaAll(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p := new Pt(3, 4); "
        "  return (case p of Pt(x, y, z) => 1, _ => 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasArityDiag(r));
}

// R34: over-binding in a case STATEMENT — was silently accepted (took the `_`
// arm); now a clean §19.3 arity diagnostic.
BEZEL_TEST(sema_class_destruct, record_destructure_over_binding_stmt_r34) {
    auto r = parseSemaAll(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p := new Pt(3, 4); var r := 0; "
        "  case p of Pt(x, y, z) => r := 1 end; "
        "  return r; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasArityDiag(r));
}

// R34: under-binding `Pt(x)` on a 2-field record is also rejected.
BEZEL_TEST(sema_class_destruct, record_destructure_under_binding_r34) {
    auto r = parseSemaAll(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p := new Pt(3, 4); "
        "  return (case p of Pt(x) => 1, _ => 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasArityDiag(r));
}

// R34: the CORRECT arity `Pt(x, y)` must NOT produce an arity diagnostic.
BEZEL_TEST(sema_class_destruct, record_destructure_correct_arity_ok_r34) {
    auto r = parseSemaAll(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p := new Pt(3, 4); "
        "  return (case p of Pt(x, y) => x + y, _ => 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!hasArityDiag(r));
}

// ── §19.3: class type-test patterns (bare name, var binding) still work ──────

// A bare class type-test pattern `Circle` (no parens) is valid. §19.3 says:
// "use a type-test pattern (`Dog`, `var d: Dog`)". Must NOT produce an error.
BEZEL_TEST(sema_class_destruct, class_type_test_pattern_ok_spec_19_3) {
    auto r = parseSemaAll(
        "module M; "
        "class Shape  public virtual method K(): int return 0; end K; end Shape; "
        "class Circle : Shape "
        "  public _r: int; "
        "  public constructor(r: int) _r := r; end constructor; "
        "  public override method K(): int return _r; end K; "
        "end Circle; "
        "class C public static method Main(): int "
        "  using var c := new Circle(5); "
        "  var shape: Shape := c; "
        "  return (case shape of "
        "    Circle => 1, "
        "    _ => 0); "
        "end Main; end C;");
    // Bare type-name pattern is valid (§19.3). No errors expected.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A typed-binding pattern `var c: Circle` on a class is valid (§19.3).
BEZEL_TEST(sema_class_destruct, class_typed_binding_pattern_ok_spec_19_3) {
    auto r = parseSemaAll(
        "module M; "
        "class Shape  public virtual method K(): int return 0; end K; end Shape; "
        "class Circle : Shape "
        "  public _r: int; "
        "  public constructor(r: int) _r := r; end constructor; "
        "  public override method K(): int return _r; end K; "
        "end Circle; "
        "class C public static method Main(): int "
        "  using var c := new Circle(5); "
        "  var shape: Shape := c; "
        "  return (case shape of "
        "    var c: Circle => c._r, "
        "    _ => 0); "
        "end Main; end C;");
    // Typed-binding pattern is valid (§19.3). No errors expected.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ── §19.3: record destructuring still works (control) ────────────────────────

// A positional record destructuring pattern `Point(x, y)` is valid per §19.3.
// This must NOT produce an error (it is a record, not a class).
BEZEL_TEST(sema_class_destruct, record_destructuring_still_ok_spec_19_3) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var p := new Point(3, 4); "
        "  return (case p of "
        "    Point(x, y) => x + y, "
        "    _ => 0); "
        "end Main; end C;");
    // Record destructuring is valid (§19.3). No errors expected.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Record destructuring with discard sub-pattern `Point(_, y)` is valid.
BEZEL_TEST(sema_class_destruct, record_destructuring_with_discard_ok_spec_19_3) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var p := new Point(10, 20); "
        "  return (case p of "
        "    Point(_, y) => y, "
        "    _ => 0); "
        "end Main; end C;");
    // §19.3: _ in positional position is a valid discard sub-pattern for records.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
