// fix #2068 / §14.1 / §27.1 (v0.204): a direct call through a function-typed
// field of a GENERIC instantiation (`b.V(42)` where `b: Box<(int) -> int>`
// from `record Box<U>(V: U)`) silently produced 0. The equivalent non-generic
// function field call (a body-declared `(int) -> int` field) worked, and
// extracting to a local first (`var g := b.V; g(42)`) also worked.
//
// Root cause: codegen's function-typed-field dispatch in lowerCall (the
// FieldSymbol arm at LlvmBackend_Calls2.cpp ~L3294) read the field's declared
// type from `fs->posParam->type` (or `fs->decl->type` for a body field) and
// looked up `ctx_.resolvedType(*…->type)`. For a generic-param field `V: U`
// instantiated with `Box<(int) -> int>`, that resolved to the BARE generic-
// param TypeRef (Named, no declaration) — not a Function kind — so the
// dispatch did not fire and the call fell through with no MethodSymbol, no
// callable resolution, and no diagnostic.
//
// Fix: when the declared type is not itself a Function, fall back to
// `ctx_.exprType(*call.callee)` — the sema-substituted callee type that
// already reflects the generic substitution (§14.1 / #361). For
// `Box<(int) -> int>` that is `(int) -> int`; codegen then routes the call
// through emitClosureCall exactly like the non-generic field case.
//
// All tests use the AOT harness (per #1861; in-process JIT retired).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// Canonical repro: `record Box<U>(V: U)` instantiated with `Box<(int) -> int>`,
// then `b.V(42)`. Was exit=0; must be 42.
const char* kRecordBox =
    "module M; record Box<U>(V: U); "
    "class C "
    "  public static method Id(x: int): int return x; end Id; "
    "  public static method Main(): int "
    "    var f: (int) -> int := Id; "
    "    using var b := new Box<(int) -> int>(f); "
    "    return b.V(42); "
    "  end Main; "
    "end C; ";
}  // namespace

// §14.1 / §27.1 (#2068): the exact repro from the issue — direct call through
// a generic-record positional function-typed field. Was 0; must be 42.
BEZEL_TEST(generic_field_direct_call_2068, record_box_int_to_int_42) {
    BEZEL_EXPECT_EQ(run(std::string(kRecordBox)), 42);
}

// §14.1 / §27.1 (#2068): the control — same call after extracting to a local
// (`var g := b.V; return g(42)`) already worked BEFORE the fix; this guards
// against a regression that would touch the local-store path.
BEZEL_TEST(generic_field_direct_call_2068,
           record_box_int_to_int_via_local_42) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<U>(V: U); "
        "class C "
        "  public static method Id(x: int): int return x; end Id; "
        "  public static method Main(): int "
        "    var f: (int) -> int := Id; "
        "    using var b := new Box<(int) -> int>(f); "
        "    var g: (int) -> int := b.V; "
        "    return g(42); "
        "  end Main; "
        "end C; "), 42);
}

// §14.1 / §27.1 (#2068): the lambda variant — `b.V` stores a lambda instead
// of a method reference. The closure-call dispatch (emitClosureCall) must
// work for the substituted type as well.
BEZEL_TEST(generic_field_direct_call_2068, record_box_lambda_call_43) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<U>(V: U); "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box<(int) -> int>(lambda(x) => x + 1); "
        "    return b.V(42); "
        "  end Main; "
        "end C; "), 43);
}

// §14.1 / §27.1 (#2068): the multi-param case — `record Pair<A, B>(first: A,
// second: B)` instantiated with `Pair<(int) -> int, int>`. The substitution
// has to land on `first` (the function-typed field), not `second` (a plain
// int). Exercises the substituteMemberType path against a non-trivial
// generic-arg vector.
BEZEL_TEST(generic_field_direct_call_2068, pair_function_field_42) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pair<A, B>(first: A, second: B); "
        "class C "
        "  public static method Id(x: int): int return x; end Id; "
        "  public static method Main(): int "
        "    var f: (int) -> int := Id; "
        "    using var p := new Pair<(int) -> int, int>(f, 99); "
        "    return p.first(42); "
        "  end Main; "
        "end C; "), 42);
}

// §14.1 / §27.1 (#2068): the CLASS variant — a generic class with a
// function-typed body field (`class Box<U> { public V: U; }`), instantiated
// with `Box<(int) -> int>`. Exercises the FieldDecl branch (not the
// posParam branch) of the fix.
BEZEL_TEST(generic_field_direct_call_2068, class_box_function_field_42) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Box<U> "
        "  public V: U := Box<U>.Noop; "
        "  public static method Noop(x: int): int return x; end Noop; "
        "  public static method Id(x: int): int return x; end Id; "
        "  public static method Main(): int "
        "    using var b := new Box<(int) -> int>(); "
        "    b.V := Id; "
        "    return b.V(42); "
        "  end Main; "
        "end Box; "), 42);
}