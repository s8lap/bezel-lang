// Codegen — §5.1 "Implicit numeric conversions" / §22.7 "Constructors"
// (v0.307), issue #4823.
//
// A NON-CONSTANT `int` argument at a `double` constructor parameter was not
// widened before the inlined constructor body used it. `Emitter::buildOrderedArgs`
// (LlvmBackend_Assign.cpp) evaluates and places constructor arguments, but a
// `new`-expression whose constructor's compiled LLVM parameter types do not
// match the placed argument types (here: an i32 argument at a declared
// `double` formal) falls back to INLINING the constructor body rather than
// emitting a real `call`. That inline path (LlvmBackend_Exprs2.cpp, the
// `new`-expression lowering's per-parameter binding loop reached once
// `typesMismatch` is true) bound each non-`ref` formal's parameter slot at
// the ARGUMENT's own LLVM type via `wrapArgForNullableValueFormal`, which
// only widens a NULLABLE-value formal (`T?`) — a plain (non-`?`) numeric
// formal fell through unwidened. The inlined body's later uses of the
// formal (a string interpolation, in the repro) then read/pass the value at
// its DECLARED width (`double`), producing a call whose actual argument
// (i32) disagreed with the callee's formal (double): `verifyModule` rejected
// the module ("Call parameter type does not match function signature!").
//
// #4046 covered only an int LITERAL argument (constant-folded to a double
// literal well before this binding loop runs, so it never reached the bug).
// This is the non-constant sibling: the argument is a `var`, so the mismatch
// survives to codegen's inline-ctor argument binding.
//
// The fix reuses `coerceExprValueToSlot` — the SAME widening helper an
// ordinary compiled method-call argument gets (LlvmBackend_Layout.cpp) —
// rather than new logic, applied to the inline-ctor per-parameter bind when
// the argument's LLVM type does not match the formal's declared LLVM type.
//
// T1 — the issue's own repro: exact-arity constructor, non-constant `int`
// argument at a `double` formal, class. Pre-fix: IR verification failure
// (bezelc: codegen error), so `runAot` never runs the program (`ran ==
// false`). Post-fix: prints "F5".
// T2 — the issue's defaulted-constructor variant (`v: double, k: int := 2`):
// same inline-ctor path, now with an omitted trailing default filled by
// `fillOmittedParamDefaults`.
// T3 — a primary-constructor (positional) record parameter typed `double`.
// T4 — a `this(...)` constructor-chain call passing the chaining
// constructor's own non-constant `int` parameter to the target `double`
// formal.
// T5 — a `base(...)` constructor-chain call, same shape, across the
// base/derived boundary.
// T6 — the same repro with a NAMED argument (`new F(v := i)`), proving the
// fix is not positional-binding-specific.

#include <string>

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Returns the program's captured stdout on a clean run, or a sentinel that
// can never equal an expected value on any failure (compile reject, crash,
// or non-zero exit) — so a stdout-value assertion fails loudly rather than
// vacuously passing when the program simply didn't run.
std::string runStdout(const std::string& src) {
    auto r = runAot(src);
    if (!r.ran || r.exitCode != 0) return std::string("<did-not-run>");
    return r.stdoutText;
}

}  // namespace

// ===========================================================================
// T1 — issue's own repro: exact-arity ctor, non-constant int -> double.
// ===========================================================================
BEZEL_TEST(codegen_ctor_non_constant_int_to_double_widen_4823,
           exact_arity_ctor_non_constant_int_arg_widens_spec_5_1) {
    BEZEL_EXPECT_EQ(
        runStdout(
            "module M; "
            "public class F4823a "
            "  public readonly property Tag: string; "
            "  public constructor(v: double) Tag := $\"F{v}\"; end constructor; "
            "end F4823a; "
            "public class C4823a "
            "  public static method Main(): int "
            "    var i: int := 5; "
            "    var f := new F4823a(i); "
            "    Console.WriteLine(f.Tag); "
            "    return 0; "
            "  end Main; "
            "end C4823a;"),
        std::string("F5\n"));
}

// ===========================================================================
// T2 — defaulted constructor: non-constant int at the leading `double`
// formal, trailing `int` formal filled from its default.
// ===========================================================================
BEZEL_TEST(codegen_ctor_non_constant_int_to_double_widen_4823,
           defaulted_ctor_non_constant_int_arg_widens_spec_5_1) {
    BEZEL_EXPECT_EQ(
        runStdout(
            "module M; "
            "public class F4823b "
            "  public readonly property Tag: string; "
            "  public constructor(v: double, k: int := 2) "
            "    Tag := $\"F{v}.{k}\"; "
            "  end constructor; "
            "end F4823b; "
            "public class C4823b "
            "  public static method Main(): int "
            "    var i: int := 5; "
            "    var f := new F4823b(i); "
            "    Console.WriteLine(f.Tag); "
            "    return 0; "
            "  end Main; "
            "end C4823b;"),
        std::string("F5.2\n"));
}

// ===========================================================================
// T3 — primary-constructor (positional) record parameter typed `double`.
// ===========================================================================
BEZEL_TEST(codegen_ctor_non_constant_int_to_double_widen_4823,
           primary_ctor_record_non_constant_int_arg_widens_spec_5_1) {
    BEZEL_EXPECT_EQ(
        runStdout(
            "module M; "
            "public record Box4823(v: double) "
            "end Box4823; "
            "public class C4823c "
            "  public static method Main(): int "
            "    var i: int := 5; "
            "    var b := new Box4823(i); "
            "    Console.WriteLine(b.v.ToString()); "
            "    return 0; "
            "  end Main; "
            "end C4823c;"),
        std::string("5\n"));
}

// ===========================================================================
// T4 — `this(...)` constructor-chain call forwarding a non-constant `int`
// to a target `double` formal.
// ===========================================================================
BEZEL_TEST(codegen_ctor_non_constant_int_to_double_widen_4823,
           this_chain_non_constant_int_arg_widens_spec_5_1) {
    BEZEL_EXPECT_EQ(
        runStdout(
            "module M; "
            "public class G4823 "
            "  public readonly property Tag: string; "
            "  public constructor(v: double) Tag := $\"G{v}\"; end constructor; "
            "  public constructor(i: int) this(i); end constructor; "
            "end G4823; "
            "public class C4823d "
            "  public static method Main(): int "
            "    var i: int := 5; "
            "    var g := new G4823(i); "
            "    Console.WriteLine(g.Tag); "
            "    return 0; "
            "  end Main; "
            "end C4823d;"),
        std::string("G5\n"));
}

// ===========================================================================
// T5 — `base(...)` constructor-chain call across the base/derived boundary.
// ===========================================================================
BEZEL_TEST(codegen_ctor_non_constant_int_to_double_widen_4823,
           base_chain_non_constant_int_arg_widens_spec_5_1) {
    BEZEL_EXPECT_EQ(
        runStdout(
            "module M; "
            "public class Base4823 "
            "  public readonly property Tag: string; "
            "  public constructor(v: double) Tag := $\"B{v}\"; end constructor; "
            "end Base4823; "
            "public class Derived4823: Base4823 "
            "  public constructor(i: int) base(i); end constructor; "
            "end Derived4823; "
            "public class C4823e "
            "  public static method Main(): int "
            "    var i: int := 5; "
            "    var d := new Derived4823(i); "
            "    Console.WriteLine(d.Tag); "
            "    return 0; "
            "  end Main; "
            "end C4823e;"),
        std::string("B5\n"));
}

// ===========================================================================
// T6 — same repro as T1, with a NAMED argument (`v := i`).
// ===========================================================================
BEZEL_TEST(codegen_ctor_non_constant_int_to_double_widen_4823,
           named_arg_non_constant_int_arg_widens_spec_5_1) {
    BEZEL_EXPECT_EQ(
        runStdout(
            "module M; "
            "public class F4823f "
            "  public readonly property Tag: string; "
            "  public constructor(v: double) Tag := $\"F{v}\"; end constructor; "
            "end F4823f; "
            "public class C4823f "
            "  public static method Main(): int "
            "    var i: int := 5; "
            "    var f := new F4823f(v := i); "
            "    Console.WriteLine(f.Tag); "
            "    return 0; "
            "  end Main; "
            "end C4823f;"),
        std::string("F5\n"));
}
