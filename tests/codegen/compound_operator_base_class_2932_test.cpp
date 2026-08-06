// Spec-derived regression test for issue #2932.
//
// §25.2 (Restrictions and resolution) / §25.1 (Overloadable operators) / §6.6
// (Compound assignments) / §12.2 (Hiding is not allowed), v0.240.
//
// THE DEFECT. A compound assignment whose §25 `operator <op>` is declared on an
// operand's BASE CLASS aborted the compile, while the plain BINARY form of the
// same expression one line above was correct. Measured on `20cc955`:
//
//     var m := n + d;   // OK — 8
//     n += d;           // ICE: "Call parameter type does not match function
//                       //  signature! … call { i32, i1 }
//                       //  @llvm.sadd.with.overflow.i32(i32 %56, ptr %55)"
//
// THE CAUSE. §25.2 "Resolution", verbatim: "For `a <op> b` on non-primitive
// operands, the candidate set is the union of the `operator <op>` overloads
// declared on the **static types of both operands, including declarations
// inherited from their base classes** (a derived-typed operand reaches its
// base's operators, as in C#: no user-facing upcast exists that could reach them
// otherwise, and [§12.2]'s no-hiding rule — which covers operator declarations
// per token, stated explicitly there — prevents a derived class from
// redeclaring an operator token its base declares, so one chain contributes no
// duplicate candidates)."
//
// `Emitter::findCompoundOperatorOverload` — the one definition #2904 extracted
// out of the `#610`/`#2156` arm of `lowerStmt` — scanned a candidate type's OWN
// members only, with no walk of `baseTypes`. So a DERIVED-typed operand
// contributed NO candidates, the lookup declined, and `emitCompoundOp`'s integer
// tail handed the class POINTER to `llvm.sadd.with.overflow`. `lowerBinary`
// walks the chain, which is why the two spellings disagreed inside one program.
//
// THE FIX walks the CLASS base chain in that one function, so every compound
// caller #2904 wired to it gains the inherited candidates at once. §12.2's
// no-hiding rule makes the first match up the chain THE match — there is no
// ambiguity to resolve — and the scan previously only ever DECLINED for these
// shapes, so widening it can turn an abort into a correct call and nothing else.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int runProg(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ===========================================================================
// T1 — #2932's repro. `operator +(int, Base)` reached through a `Derived`
// operand, in BOTH spellings: the binary form (correct before the fix, kept
// here so a green result cannot come from the compound form being skipped) and
// the compound form. `1 + 7` is 8 twice, so 8*10 + 8 = 88.
// Pre-fix: the compile ABORTED.
// ===========================================================================
BEZEL_TEST(codegen_compound_operator_base_class_2932, inherited_operator_25_2) {
    BEZEL_EXPECT_EQ(runProg(
        "module B2932; "
        "public class Base "
        "  public v: int := 0; "
        "  public static method operator +(x: int, y: Base): int "
        "    return x + y.v; "
        "  end; "
        "end Base; "
        "public class Derived: Base "
        "end Derived; "
        "public class M "
        "  public static method Main(): int "
        "    using var d := new Derived(); "
        "    d.v := 7; "
        "    var n: int := 1; "
        "    var m := n + d; "
        "    n += d; "
        "    return m * 10 + n; "
        "  end Main; "
        "end M;"), 88);
}

// ===========================================================================
// T2 — the same inherited operator through an ACCESSOR-mediated target, which
// is where #2904 wired the shared lookup in. A two-level chain (`Mid: Base`,
// `Leaf: Mid`) pins that the walk is transitive, not one hop.
// Pre-fix: the compile ABORTED.
// ===========================================================================
BEZEL_TEST(codegen_compound_operator_base_class_2932, inherited_operator_through_property_25_2) {
    BEZEL_EXPECT_EQ(runProg(
        "module B2932b; "
        "public class Base "
        "  public v: int := 0; "
        "  public static method operator +(x: int, y: Base): int "
        "    return x + y.v; "
        "  end; "
        "end Base; "
        "public class Mid: Base "
        "end Mid; "
        "public class Leaf: Mid "
        "end Leaf; "
        "public class Box "
        "  public _n: int := 1; "
        "  public property NP: int "
        "    get => _n; "
        "    set => do _n := value; end; "
        "  end; "
        "end Box; "
        "public class M "
        "  public static method Main(): int "
        "    using var lf := new Leaf(); "
        "    lf.v := 7; "
        "    using var b := new Box(); "
        "    b.NP += lf; "
        "    return b.NP; "
        "  end Main; "
        "end M;"), 8);
}

// ===========================================================================
// T3 — OVER-ACCEPTANCE CONTROL. The base walk must not invent a candidate where
// §25.2's in-order parameter match does not hold: an `operator +(int, Other)`
// on the base is NOT applicable to `n += d` with `d: Derived` (param1 names a
// different declaration), so the statement must be a sourced REJECT, not a
// silent dispatch to the wrong operator and not an IR-verifier abort.
// ===========================================================================
BEZEL_TEST(codegen_compound_operator_base_class_2932, unrelated_base_operator_rejected_25_2) {
    auto r = runAot(
        "module B2932c; "
        "public class Other "
        "  public w: int := 0; "
        "end Other; "
        "public class Base "
        "  public v: int := 0; "
        "  public static method operator +(x: int, y: Other): int "
        "    return x + y.w; "
        "  end; "
        "end Base; "
        "public class Derived: Base "
        "end Derived; "
        "public class M "
        "  public static method Main(): int "
        "    using var d := new Derived(); "
        "    d.v := 7; "
        "    var n: int := 1; "
        "    n += d; "
        "    return n; "
        "  end Main; "
        "end M;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ===========================================================================
// T4 — OVER-ACCEPTANCE CONTROL for the ordinary case: an operator declared on
// the operand's OWN type must still be found, and an ordinary numeric compound
// must be untouched by the base walk. Passes both ways by construction.
// ===========================================================================
BEZEL_TEST(codegen_compound_operator_base_class_2932, own_type_operator_unchanged_control) {
    BEZEL_EXPECT_EQ(runProg(
        "module B2932d; "
        "public record Scale "
        "  public k: int; "
        "  public static method operator *(x: int, s: Scale): int "
        "    return x * s.k; "
        "  end; "
        "end Scale; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var n: int := 6; "
        "    var s := new Scale(k := 7); "
        "    n *= s; "
        "    if n = 42 then acc := acc + 1; end; "
        "    var q: int := 10; "
        "    q += 5; "
        "    if q = 15 then acc := acc + 2; end; "
        "    return acc; "
        "  end Main; "
        "end M;"), 3);
}
