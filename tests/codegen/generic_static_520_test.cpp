// Codegen — generic class static method access via explicit type args (issue #520).
//
// §14.1 / §22.12 / Appendix A:
//   The grammar lacked a production for `Identifier<TypeArgs>` in expression
//   position (as a receiver for a subsequent `.member` access). This prevented
//   the common static-factory idiom `Registry<int>.Register(...)`.
//
// Grammar change (§A.5): a primary IdentifierExpr may be followed immediately
// by `< TypeArgList >` before the postfix chain, producing an IdentifierExpr
// with a non-empty `typeArgs` vector that sema interprets as an explicit
// generic-type instantiation for static member access.
//
// Disambiguation: a bounded lookahead (≤64 tokens) scans for `> .` (dot
// follows the closing `>`) to distinguish a genuine type-arg list from a
// relational `<` expression.  The #509 lookahead that detects `> (` is reused
// for method-level type args on the member name (`obj.Method<T>(...)`);
// the new `> .` variant handles the receiver-position case introduced here.
//
// Sema: when an IdentifierExpr carries non-empty typeArgs, the name is first
// resolved to a TypeSymbol (generic class), then the type args are resolved and
// bound to the TypeDecl's genericParams to form a synthetic instantiation
// TypeRef (kind=Named, declaration=TypeSymbol, genericArgs populated).  That
// TypeRef is stored as the exprType of the IdentifierExpr so that the
// subsequent `.member` MemberAccessExpr can resolve the static member against
// the concretely instantiated generic class.
//
// Codegen: the receiver-substitution block (previously guarded with
// `!isStaticMethod(ms)`) is extended to also handle the case where the
// receiver is a generic-type IdentifierExpr with explicit type args.  The
// genericSubst_/genericSubstSemaTy_ maps are seeded from the IdentifierExpr's
// typeArgs so that the generic method body is monomorphized with the correct
// concrete type.
//
// Spec references:
//   §14.1 — generic methods: monomorphized at each call site with substituted
//            return type and body.
//   §22.12 — static methods on generic classes are valid and accessible via
//             the instantiated type name.
//   Appendix A — grammar: PrimaryExpression extended to allow explicit type
//                args on an identifier receiver.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Helper: compile+JIT and return exit code; -999 on compile/JIT failure.
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// Helper: compile+JIT and return the full JitResult (for checking diagnostics).
bezel::test::JitResult jit(const std::string& prog) {
    return runAot(prog);
}

// Generic class with a static method that uses T.
// §14.1 / §22.12: `Registry<T>` has a static method `Make(v: T): T`.
// Called as `Registry<int>.Make(7)` → must return 7.
const char* kRegistry =
    "module M; "
    "class Registry<T> "
    "  public static method Make(v: T): T "
    "    return v; "
    "  end Make; "
    "end Registry; ";

}  // namespace

// ── Primary: static method on a generic class, explicit int type arg ──────────

// §14.1 / §22.12 / #520: `Registry<int>.Make(7)` must return 7.
// Before fix: the parser fails with a "parse error" or misparses `Registry<int>`
// as a relational expression, so no code is generated.
BEZEL_TEST(codegen_generic_static_520, make_int_returns_value_14_1_22_12) {
    BEZEL_EXPECT_EQ(run(
        std::string(kRegistry) +
        "class C "
        "  public static method Main(): int "
        "    return Registry<int>.Make(7); "
        "  end Main; "
        "end C;"), 7);
}

// §14.1 / §22.12 / #520: return value 42 — sanity check with a different constant.
BEZEL_TEST(codegen_generic_static_520, make_int_42_14_1_22_12) {
    BEZEL_EXPECT_EQ(run(
        std::string(kRegistry) +
        "class C "
        "  public static method Main(): int "
        "    var x := Registry<int>.Make(42); "
        "    return x; "
        "  end Main; "
        "end C;"), 42);
}

// §14.1 / §22.12 / #520: distinct instantiations use DIFFERENT monomorphizations.
// `Foo<int>.Make(3)` and `Foo<string>.Make("hi")` must not share a cache slot
// that overrides each other's substitution.
BEZEL_TEST(codegen_generic_static_520, two_instantiations_no_cache_collision_14_1_22_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Foo<T> "
        "  public static method Make(v: T): T "
        "    return v; "
        "  end Make; "
        "end Foo; "
        "class C "
        "  public static method Main(): int "
        "    var n := Foo<int>.Make(99); "
        "    var s := Foo<string>.Make(\"ok\"); "
        "    if s = \"ok\" then return n; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 99);
}

// §22.12 / #520: static method on a generic class that accumulates a value.
// The return type is int (not T), but T must be bound for the body.
BEZEL_TEST(codegen_generic_static_520, static_with_int_return_not_t_14_1_22_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Acc<T> "
        "  public static _n: int := 0; "
        "  public static method Add(v: int): int "
        "    _n := _n + v; "
        "    return _n; "
        "  end Add; "
        "end Acc; "
        "class C "
        "  public static method Main(): int "
        "    Acc<int>.Add(10); "
        "    Acc<int>.Add(20); "
        "    return Acc<int>.Add(12); "
        "  end Main; "
        "end C;"), 42);
}

// ── String type arg ─────────────────────────────────────────────────────────

// §14.1 / §22.12 / #520: `Registry<string>.Make("hello")` must return "hello".
BEZEL_TEST(codegen_generic_static_520, make_string_round_trip_14_1_22_12) {
    BEZEL_EXPECT_EQ(run(
        std::string(kRegistry) +
        "class C "
        "  public static method Main(): int "
        "    var s := Registry<string>.Make(\"hello\"); "
        "    if s = \"hello\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// ── Parse diagnostics: no errors for well-formed code ──────────────────────

// §14.1 / #520: the compiler must emit NO errors for `Registry<int>.Make(7)`.
BEZEL_TEST(codegen_generic_static_520, no_parse_error_for_valid_code_14_1) {
    auto r = jit(
        std::string(kRegistry) +
        "class C "
        "  public static method Main(): int "
        "    return Registry<int>.Make(7); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ── Control: non-generic Type.StaticMethod() still works ───────────────────

// §22 / #520 regression guard: a plain non-generic static call `C.Add(2, 3)`
// must still compile and return 5. The new disambiguation must not interfere
// with non-generic static access.
BEZEL_TEST(codegen_generic_static_520, control_nongeneric_static_still_works_22) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Util "
        "  public static method Add(a: int, b: int): int => a + b; "
        "end Util; "
        "class C "
        "  public static method Main(): int => Util.Add(2, 3); "
        "end C;"), 5);
}

// ── Control: explicit method-level type args (#509) still work ────────────────

// §14.1 / #509 regression guard: `Wrapper.Identity<int>(42)` (method-level
// type args, NOT class-level) must still compile and return 42. The new `> .`
// disambiguation must not break the `> (` disambiguation used by #509.
BEZEL_TEST(codegen_generic_static_520, control_method_typeargs_509_still_works_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Wrapper "
        "  public static method Identity<T>(v: T): T "
        "    return v; "
        "  end Identity; "
        "end Wrapper; "
        "class C "
        "  public static method Main(): int "
        "    return Wrapper.Identity<int>(42); "
        "  end Main; "
        "end C;"), 42);
}

// ── Control: relational expressions must NOT be misinterpreted ───────────────

// §17.1 / #520 regression guard: `a < b` and `a < b > c` (chain) must still
// parse as relational operators, not be swallowed as type-arg lists.  The
// disambiguation checks for `> .` (dot) after the closing `>`; a relational
// chain has a numeric literal or expression there, so it is classified
// correctly as a relational operator.
BEZEL_TEST(codegen_generic_static_520, control_relational_lt_not_misinterpreted_17_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var a: int := 3; "
        "    var b: int := 5; "
        "    if a < b then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

BEZEL_TEST(codegen_generic_static_520, control_relational_gt_not_misinterpreted_17_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var a: int := 10; "
        "    var b: int := 2; "
        "    if a > b then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// §17.1 / #520: `x < y` where both are expression identifiers (not type names)
// must not be misinterpreted as a type-arg list.  The disambiguation rejects
// because after a well-formed `>` the next token is not `.`; it is either
// another operator, a statement-terminator keyword, or EOF.
BEZEL_TEST(codegen_generic_static_520, control_ident_relational_not_misinterpreted_17_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var x: int := 1; "
        "    var y: int := 2; "
        "    if x < y then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}
