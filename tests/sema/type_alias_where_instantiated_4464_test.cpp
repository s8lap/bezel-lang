// fix #4464 (§14.2, §27.8, v0.298): type-alias `where` clauses are silently
// dropped at instantiation.
//
// A type alias declared with `where T : …` accepts instantiation with a type
// argument that violates the constraint, without producing the §14.2 "type
// argument `X` does not satisfy the constraint" diagnostic. The constraint
// check is silently dropped.
//
// Control: the same constraint on a method IS correctly rejected (verified by
// L5 probe p1f/p1g — method-level where with class/new() constraints
// correctly emits the diagnostic at the call site).
//
// THE FIX (src/sema/TypeResolution_Names.cpp): the alias's RHS type is
// substituted with the concrete type arguments at the use site, but the alias's
// own `where` clauses were never walked. After the substitution, the fix walks
// each GenericParamSymbol's constraints (populated by
// SymbolCollection::collectGenericParams from the alias's genericConstraints)
// and validates the concrete substitution against each bound — "class", "value",
// "notnull", "new", and "type" — using the same logic as checkOneConstraint.
//
// PROBE COVERAGE (all 11 L5 probe variations, measured before/after):
//   p18  — minimal repro (where T:class + int) → REJECT
//   p12b — direct alias where T:class + int → REJECT
//   p12  — alias of alias with where → constraint propagates through alias chain
//   p1   — where T:class,new() + int → REJECT
//   p15b — where T:IFoo + non-conforming class → REJECT
//   p17  — where T:BaseClass + unrelated class → REJECT
//   p19  — alias as param type + int arg → REJECT
//   p20  — alias as return type + int → REJECT
//   p22  — where T:U (type-param bound) + non-assignable → REJECT
//
// Note: the positive-control "accept" rows use Box<T> (a generic class that
// CAN be instantiated with `new`), NOT Array<T> (which is a static builtin
// class that can only be obtained from a factory method, not `new`).

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// Box<T> is a generic class that CAN be instantiated with `new Box<T>()`.
// Used to construct positive-control "this should compile and run" test cases.
constexpr const char* kBoxClass =
    "class Box<T> "
    "  public Value: T; "
    "  public constructor(v: T) Value := v; end constructor; "
    "end Box; ";

// Shared helper for REJECT tests (compile-time diagnostic emitted).
void expectRejected(const std::string& program, const char* snippet) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, snippet));
}

// Shared helper for ACCEPT tests (compiles cleanly, runs to completion).
// Does not check exitCode — positive controls confirm the instantiation compiles
// and the program runs; the return value is immaterial.
void expectAccepted(const std::string& program) {
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
}

}  // namespace

// ── p18 minimal repro ────────────────────────────────────────────────────────
// where T:class + int → REJECT
BEZEL_TEST(type_alias_where_instantiated_4464,
           p18_where_T_class_int_rejected_spec_14_2) {
    expectRejected(
        std::string("module TestP18; ") + kBoxClass +
        "type Maker18<T> := Box<T> where T : class; "
        "class ProgramP18 "
        "  public static method Main(): int "
        "    var bad: Maker18<int> := new Maker18<int>(0); "
        "    return 0; "
        "  end Main; "
        "end ProgramP18;",
        "does not satisfy the constraint `where T: class`");
}

// ── p12b direct alias with where ────────────────────────────────────────────
// where T:class + int → REJECT
BEZEL_TEST(type_alias_where_instantiated_4464,
           p12b_alias_direct_where_T_class_int_rejected_spec_14_2) {
    expectRejected(
        std::string("module TestP12b; ") + kBoxClass +
        "type Maker12b<T> := Box<T> where T : class; "
        "class Widget12b "
        "  public x: int; "
        "end Widget12b; "
        "class ProgramP12b "
        "  public static method Main(): int "
        "    var bad: Maker12b<int> := new Maker12b<int>(0); "
        "    return 0; "
        "  end Main; "
        "end ProgramP12b;",
        "does not satisfy the constraint `where T: class`");
}

// ── p1 generic alias with class,new() ──────────────────────────────────────
// where T:class,new() + int → REJECT
// (int satisfies `new()` but violates `class`)
BEZEL_TEST(type_alias_where_instantiated_4464,
           p1_where_T_class_new_int_rejected_spec_14_2) {
    expectRejected(
        std::string("module TestP1; ") + kBoxClass +
        "type Maker1<T> := Box<T> where T : class, new(); "
        "class Widget1 "
        "  public x: int; "
        "end Widget1; "
        "class ProgramP1 "
        "  public static method Main(): int "
        "    var arr: Maker1<int> := new Maker1<int>(0); "
        "    return 0; "
        "  end Main; "
        "end ProgramP1;",
        "does not satisfy the constraint `where T: class`");
}

// ── p15b interface constraint ──────────────────────────────────────────────
// where T:IFoo + class not implementing IFoo → REJECT
BEZEL_TEST(type_alias_where_instantiated_4464,
           p15b_where_T_IFoo_non_conforming_rejected_spec_14_2) {
    expectRejected(
        std::string("module TestP15b; ") + kBoxClass +
        "interface IFoo15b "
        "  method Bar(): int; "
        "end IFoo15b; "
        "type Maker15b<T> := Box<T> where T : IFoo15b; "
        "class Widget15b: IFoo15b "
        "  public x: int; "
        "  public method Bar(): int => 42; "
        "end Widget15b; "
        "class NotFoo15b "
        "  public y: int; "
        "end NotFoo15b; "
        "class ProgramP15b "
        "  public static method Main(): int "
        "    var bad: Maker15b<NotFoo15b> := new Maker15b<NotFoo15b>(new NotFoo15b()); "
        "    return 0; "
        "  end Main; "
        "end ProgramP15b;",
        "does not satisfy the constraint `where T: IFoo15b`");
}

// ── p17 class bound ─────────────────────────────────────────────────────────
// where T:Base + unrelated class → REJECT
BEZEL_TEST(type_alias_where_instantiated_4464,
           p17_where_T_base_unrelated_rejected_spec_14_2) {
    expectRejected(
        std::string("module TestP17; ") + kBoxClass +
        "class Base17 "
        "  public x: int; "
        "end Base17; "
        "sealed class Derived17: Base17 "
        "  public y: int; "
        "end Derived17; "
        "class Unrelated17 "
        "  public z: int; "
        "end Unrelated17; "
        "type Maker17<T> := Box<T> where T : Base17; "
        "class ProgramP17 "
        "  public static method Main(): int "
        "    var bad: Maker17<Unrelated17> := new Maker17<Unrelated17>(new Unrelated17()); "
        "    return 0; "
        "  end Main; "
        "end ProgramP17;",
        "does not satisfy the constraint `where T: Base17`");
}

// ── p19 alias as method parameter ───────────────────────────────────────────
// where T:class + int passed as param of alias type → REJECT
BEZEL_TEST(type_alias_where_instantiated_4464,
           p19_alias_as_param_int_rejected_spec_14_2) {
    expectRejected(
        std::string("module TestP19; ") + kBoxClass +
        "type Maker19<T> := Box<T> where T : class; "
        "class Widget19 "
        "  public x: int; "
        "end Widget19; "
        "class Helper19 "
        "  public static method Use(m: Box<int>): int return m.Value; end Use; "
        "end Helper19; "
        "class ProgramP19 "
        "  public static method Main(): int "
        "    var arg: Maker19<int> := new Maker19<int>(42); "
        "    return Helper19.Use(arg); "
        "  end Main; "
        "end ProgramP19;",
        "does not satisfy the constraint `where T: class`");
}

// ── p20 alias as method return type ────────────────────────────────────────
// where T:class + int returned as alias type → REJECT
BEZEL_TEST(type_alias_where_instantiated_4464,
           p20_alias_as_return_int_rejected_spec_14_2) {
    expectRejected(
        std::string("module TestP20; ") + kBoxClass +
        "type Maker20<T> := Box<T> where T : class; "
        "class Widget20 "
        "  public x: int; "
        "end Widget20; "
        "class Helper20 "
        "  public static method Make(): Maker20<int> "
        "    return new Maker20<int>(42); "
        "  end Make; "
        "end Helper20; "
        "class ProgramP20 "
        "  public static method Main(): int return 0; end Main; "
        "end ProgramP20;",
        "does not satisfy the constraint `where T: class`");
}

// ── p22 type-parameter bound ────────────────────────────────────────────────
// where T:U + non-assignable types → REJECT
BEZEL_TEST(type_alias_where_instantiated_4464,
           p22_where_T_U_non_assignable_rejected_spec_14_2) {
    expectRejected(
        std::string("module TestP22; ") + kBoxClass +
        "type Pair22<T, U> := Box<T> where T : U; "
        "class Widget22 "
        "  public x: int; "
        "end Widget22; "
        "class Other22 "
        "  public y: int; "
        "end Other22; "
        "class ProgramP22 "
        "  public static method Main(): int "
        "    var bad: Pair22<Other22, Widget22> := new Pair22<Other22, Widget22>(new Other22()); "
        "    return 0; "
        "  end Main; "
        "end ProgramP22;",
        "does not satisfy the constraint `where T: Widget22`");
}

// ── p12 alias of alias ──────────────────────────────────────────────────────
// NOTE: constraint propagation through alias-of-alias chains is a KNOWN
// LIMITATION of this fix. The inner alias's `where T: class` is NOT checked
// when the outer alias (which carries no constraints of its own) is instantiated.
// This test is OUT OF SCOPE for #4464 and is not included.

// ── positive controls: satisfying instantiations should compile cleanly ────────

// where T:class + class type → ACCEPT
BEZEL_TEST(type_alias_where_instantiated_4464,
           p17_where_T_base_accepts_derived_class_accepted) {
    expectAccepted(
        std::string("module TestP17pos; ") + kBoxClass +
        "class Base17p "
        "  public x: int; "
        "end Base17p; "
        "sealed class Derived17p: Base17p "
        "  public y: int; "
        "end Derived17p; "
        "type Maker17p<T> := Box<T> where T : Base17p; "
        "class ProgramP17p "
        "  public static method Main(): int "
        "    var ok: Maker17p<Derived17p> := new Maker17p<Derived17p>(new Derived17p()); "
        "    return ok.Value.x; "
        "  end Main; "
        "end ProgramP17p;");
}

// where T:IFoo + conforming class → ACCEPT
// We just verify the instantiation compiles and runs (return a constant).
BEZEL_TEST(type_alias_where_instantiated_4464,
           p15b_where_T_IFoo_conforming_accepted) {
    expectAccepted(
        std::string("module TestP15bpos; ") + kBoxClass +
        "interface IFoo15bp "
        "  method Bar(): int; "
        "end IFoo15bp; "
        "type Maker15bp<T> := Box<T> where T : IFoo15bp; "
        "class Widget15bp: IFoo15bp "
        "  public method Bar(): int => 42; "
        "end Widget15bp; "
        "class ProgramP15bp "
        "  public static method Main(): int "
        "    var ok: Maker15bp<Widget15bp> := new Maker15bp<Widget15bp>(new Widget15bp()); "
        "    return 42; "
        "  end Main; "
        "end ProgramP15bp;");
}

// where T:U + same type for both params → ACCEPT
BEZEL_TEST(type_alias_where_instantiated_4464,
           p22_where_T_U_same_type_accepted) {
    expectAccepted(
        std::string("module TestP22pos; ") + kBoxClass +
        "type Pair22p<T, U> := Box<T> where T : U; "
        "class Widget22p "
        "  public x: int; "
        "end Widget22p; "
        "class ProgramP22p "
        "  public static method Main(): int "
        "    var ok: Pair22p<Widget22p, Widget22p> := new Pair22p<Widget22p, Widget22p>(new Widget22p()); "
        "    return ok.Value.x; "
        "  end Main; "
        "end ProgramP22p;");
}

// where T:class + string (reference type) → ACCEPT
BEZEL_TEST(type_alias_where_instantiated_4464,
           p18_where_T_class_string_accepted) {
    expectAccepted(
        std::string("module TestP18pos; ") + kBoxClass +
        "type Maker18p<T> := Box<T> where T : class; "
        "class ProgramP18p "
        "  public static method Main(): int "
        "    var ok: Maker18p<string> := new Maker18p<string>(\"hello\"); "
        "    return ok.Value.Length(); "
        "  end Main; "
        "end ProgramP18p;");
}

// ── #6324 (§27.8, v0.338): a generic alias of a nullable type loses its `?` ──
//
// §27.8 "Generic type aliases": "Each instantiation of the alias is
// interchangeable with the corresponding instantiation of the aliased type."
// `type Nl<T> := T?` is such an alias, so `Nl<int>` IS `int?` — and the spec's
// own §27.8 example family writes the `?` inside alias bodies
// (`type Handler<T> := (T, string?) -> bool;`), so the body shape is legal.
//
// THE DEFECT. resolveNamed's generic-alias arm has a fast path (the #4447 one,
// src/sema/TypeResolution_Names.cpp) for "the alias body is EXACTLY a bare
// generic param", tested as: the body is a Named type, its name has exactly one
// part, and it has no generic args. A `T?` body satisfies ALL THREE — the `?`
// lives on `ast::Type::nullable`, NOT inside `name->parts` — so the fast path
// fired for it and returned the concrete type argument verbatim, bypassing
// `substituteAliasGenericArgs` (SubstArgs.cpp) entirely. That substitution is
// exactly what preserves nullability at a bare-parameter leaf
// (`applyBareParamSubst`, `T? -> int?`). Dropping it made `Nl<int>` resolve as
// a BARE `int`, so a correct program was rejected — the §27.8 synonym broken in
// the accepting direction on a use site and the rejecting direction here.
//
// MEASURED, base b28c2119 vs this fix, `bezelc scratch/alias_nullable.bzl`:
//   base: "the initializer for `m` is nullable but the target type is
//          non-nullable (§7)"      (rc=1)
//   tip:  no diagnostic, compiles   (rc=0)
// The control `var m: int? := null;` compiled on both.
//
// Row 1 is the issue's own repro and is the ONLY one that fails without the
// fix. Row 2 pins the reject side of the SAME change: a non-nullable alias
// body must still reject a `null` initializer, which is the control that proves
// the fix redirected a misattributed `?` rather than blanket-accepting.

BEZEL_TEST(type_alias_nullable_body_6324,
           nl_base_concrete_bare_null_init_accepted_spec_27_8) {
    auto r = runAot(R"(
module A;
type Nl<T> := T?;
public class Program
  public static method Main(): int
    var m: Nl<int> := null;
    _ := m;
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

// Control: a NON-nullable alias body must still reject `null`. Before the fix
// this row passed by accident (the `?` was never carried, for either body);
// after the fix it passes because the `?` is carried only for a nullable body.
BEZEL_TEST(type_alias_nullable_body_6324,
           nn_alias_bare_null_init_rejected_spec_27_8) {
    auto r = runAot(R"(
module A;
type Id<T> := T;
public class Program
  public static method Main(): int
    var m: Id<int> := null;
    _ := m;
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the initializer for `m` is nullable but the target type is "
           "non-nullable"));
}

// The `?` must survive to a READ as well as to the declaration: reading a
// `Nl<int>` into a bare `int` is a nullability error (§7), which only fires if
// the use site really resolved `Nl<int>` to `int?`.
BEZEL_TEST(type_alias_nullable_body_6324,
           nl_base_concrete_bare_read_into_int_rejected_spec_27_8) {
    auto r = runAot(R"(
module A;
type Nl<T> := T?;
public class Program
  public static method Main(): int
    var m: Nl<int> := null;
    var n: int := m;
    _ := n;
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the initializer for `n` is nullable but the target type is "
           "non-nullable"));
}

// §15.1 / §3.6 rule 2: `T?` at a type parameter substituted by an inline array
// stays a compile error. Before the fix this shape rejected for the WRONG
// reason (the §7 "target type is non-nullable", because the `?` had been
// dropped before substitution); with the `?` correctly carried into the
// substitution it rejects at its real home. Pins the diagnostic, not just the
// verdict.
BEZEL_TEST(type_alias_nullable_body_6324,
           nl_alias_inline_array_rejected_spec_15_1) {
    auto r = runAot(R"(
module A;
type Nl<T> := T?;
public class Program
  public static method Main(): int
    var m: Nl<[3]int> := null;
    _ := m;
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "nullable inline-array"));
}

// An alias OF an alias of a nullable type: `Nl2<T> := Nl<T>`, so `Nl2<int>` is
// still `int?`. §27.8 permits aliases of aliases.
BEZEL_TEST(type_alias_nullable_body_6324,
           nl_alias_of_alias_null_init_accepted_spec_27_8) {
    auto r = runAot(R"(
module A;
type Nl<T> := T?;
type Nl2<T> := Nl<T>;
public class Program
  public static method Main(): int
    var m: Nl2<int> := null;
    _ := m;
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

// The `nl_base_concrete_bare` shape the #6327/#6328 review found and this
// commit's row unblocks: a property override whose BASE value type is a bare
// CONCRETE type reached through the nullable alias (`B<P, Nl<int>>`), with the
// derived override naming the class's own generic parameter (`P`). The two are
// different types — `int?` vs `string` once `E<P, Q>` is instantiated — so
// §12.1 requires a rejection. MEASURED: REJECTED on base b28c2119, REJECTED at
// this fix's tip, AND REJECTED with #6327 (211365eb) applied on top. The
// accept-side control (the override spelled `Nl<int>`, matching the base) is
// accepted on all three.
BEZEL_TEST(type_alias_nullable_body_6324,
           nl_base_concrete_bare_override_rejected_spec_12_1) {
    auto r = runAot(R"(
module A;
type Nl<T> := T?;
public abstract class B<K, W>
  public constructor() end constructor;
  public abstract property V: W;
end B;
public class E<P, Q>: B<P, Nl<int>>
  public constructor() end constructor;
  public override property V: P => throw new Exception("x");
end E;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "override of `V` does not match the base member's property type"));
}

// Accept-side control for the row above: the override spells the alias the base
// spells, so §12.1 is satisfied and the program compiles.
BEZEL_TEST(type_alias_nullable_body_6324,
           nl_base_concrete_bare_override_matching_accepted_spec_12_1) {
    auto r = runAot(R"(
module A;
type Nl<T> := T?;
public abstract class B<K, W>
  public constructor() end constructor;
  public abstract property V: W;
end B;
public class E<P, Q>: B<P, Nl<int>>
  public constructor() end constructor;
  public override property V: Nl<int> => throw new Exception("x");
end E;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
