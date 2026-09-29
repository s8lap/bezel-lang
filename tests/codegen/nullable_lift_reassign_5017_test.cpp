// Regression tests for issue #5017 (site A) — §5.1 "Nullable conversions",
// v0.307: "`S?` implicitly converts to `T?` whenever `S` implicitly converts
// to `T` — a present value converts by the underlying conversion, `null`
// converts to `null` (`int?` → `long?`)", at a REASSIGNMENT (`x := ki;` and
// `x ??= ki;`) into an existing nullable-value slot.
//
// The `:=` / `??=` preamble in LlvmBackend_Stmts.cpp widened only a BARE
// scalar RHS (fix #5005). A nullable-value RHS lowers to the tagged
// `{i8 hasValue; S}` struct, which that widening leaves untouched, and the
// CG-nullable wrap block matches only a bare payload — so `{i8, i32}` was
// stored over the `{i8, double}` slot, leaving the payload's high bytes at
// their prior value. MEASURED on origin/main e3b0bdf0: `x := ki;` with
// `ki: int? := 4`, `x: double?` printed `lift 0` instead of `lift 4`; the
// same with `??=`, through a `byte? → int? → double?` chain, and into a
// class field. The declaration-initializer form (`var x: double? := ki;`)
// was already correct (#840).
//
// The null-source rows pin the second half of the §5.1 sentence ("`null`
// converts to `null`"): a parked earlier fix for this issue rewrapped with
// hasValue = 1 unconditionally and turned a null source into a present `0`.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

void expectPrints(const std::string& src, const std::string& expected) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// per §5.1 (v0.307), fix #5017 — the issue's wrong-value repro verbatim.
// Pre-fix: "lift 0\n".
BEZEL_TEST(nullable_lift_reassign_5017, issue_repro_int_nullable_to_double_nullable) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method Main() "
        "    var ki: int? := 4; "
        "    var x: double? := null; "
        "    x := ki; "
        "    if x = null then "
        "      Console.WriteLine(\"lift null\"); "
        "    else "
        "      var v: double := x!; "
        "      Console.WriteLine($\"lift {v}\"); "
        "    end if; "
        "  end Main; "
        "end C;",
        "lift 4\n");
}

// per §5.1 / §7.7 (v0.307), fix #5017 — the `??=` arm has its own coercion
// block. Pre-fix: "coaleq 0\n".
BEZEL_TEST(nullable_lift_reassign_5017, coalesce_assign_int_nullable_to_double_nullable) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method Main() "
        "    var ki: int? := 4; "
        "    var x: double? := null; "
        "    x ??= ki; "
        "    var v: double := x!; "
        "    Console.WriteLine($\"coaleq {v}\"); "
        "  end Main; "
        "end C;",
        "coaleq 4\n");
}

// per §5.1 (v0.307), fix #5017 — a chain of lifted widenings, each step a
// reassignment (the triage's ctrl4 verbatim, `byte? → int? → double?`), and
// a `byte? → int? → long?` chain whose unsigned source must zero-extend
// (`byte` 200 stays 200). Pre-fix: "chain 0\n...".
BEZEL_TEST(nullable_lift_reassign_5017, chained_byte_int_double_nullable) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method Main() "
        "    var kbyte: byte? := 4; "
        "    var kint: int? := null; "
        "    kint := kbyte; "
        "    var x: double? := null; "
        "    x := kint; "
        "    var v: double := x!; "
        "    Console.WriteLine($\"chain {v}\"); "
        "    var b: byte? := 200; "
        "    var i: int? := null; "
        "    i := b; "
        "    var l: long? := null; "
        "    l := i; "
        "    var w: long := l!; "
        "    Console.WriteLine($\"chain {w}\"); "
        "  end Main; "
        "end C;",
        "chain 4\nchain 200\n");
}

// per §5.1 (v0.307), fix #5017 — an unsigned source converts by its own
// signedness (`uint` 4000000000 → `long`, not a negative sign-extension).
// The target starts at -1 so a partial (payload-low-bytes-only) store —
// the pre-fix shape — cannot read back the right value by accident.
BEZEL_TEST(nullable_lift_reassign_5017, unsigned_source_zero_extends) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method Main() "
        "    var u: uint? := 4000000000; "
        "    var x: long? := -1; "
        "    x := u; "
        "    var v: long := x!; "
        "    Console.WriteLine($\"u {v}\"); "
        "  end Main; "
        "end C;",
        "u 4000000000\n");
}

// per §5.1 (v0.307), fix #5017 — a class-field target reaches the same
// `:=` preamble through `lvalueAddr`; a later null source must store null.
BEZEL_TEST(nullable_lift_reassign_5017, field_target_present_then_null) {
    expectPrints(
        "module M; "
        "public class Box "
        "  public v: double? := null; "
        "end Box; "
        "public class C "
        "  public static method Main() "
        "    var b := new Box(); "
        "    var ki: int? := 7; "
        "    b.v := ki; "
        "    var p: double := b.v!; "
        "    Console.WriteLine($\"f {p}\"); "
        "    var kn: int? := null; "
        "    b.v := kn; "
        "    if b.v = null then Console.WriteLine(\"f null\"); "
        "    else Console.WriteLine(\"f present\"); end if; "
        "  end Main; "
        "end C;",
        "f 7\nf null\n");
}

// per §5.1 (v0.307: "`null` converts to `null`"), fix #5017 — a null
// source keeps the target null through `:=`, `??=`, and the same-width
// case, including a target that previously held a present value.
BEZEL_TEST(nullable_lift_reassign_5017, null_source_stays_null) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method Main() "
        "    var kn: int? := null; "
        "    var x: double? := 9.5; "
        "    x := kn; "
        "    if x = null then Console.WriteLine(\"a null\"); "
        "    else Console.WriteLine(\"a present\"); end if; "
        "    var y: double? := null; "
        "    y ??= kn; "
        "    if y = null then Console.WriteLine(\"b null\"); "
        "    else Console.WriteLine(\"b present\"); end if; "
        "    var z: int? := 3; "
        "    z := kn; "
        "    if z = null then Console.WriteLine(\"c null\"); "
        "    else Console.WriteLine(\"c present\"); end if; "
        "  end Main; "
        "end C;",
        "a null\nb null\nc null\n");
}
