// Spec-derived regression test for issue #8141 — an EXPRESSION-bodied method
// that returns a stored function value (`Get(): (int)->int => this.Fn`)
// over-released the field's closure cell when its result was bound to a local
// (`var g := h.Get();`): a heap-use-after-free on the next read of the field,
// freed by `bz_closure_release`, the cell allocated in `Holder$ctor`. The
// BLOCK-bodied twin (`return this.Fn;`) and the PROPERTY getter
// (`property G => this.Fn`) of the SAME program were clean.
//
// THE SPEC (v0.355).
//
// §22.2 "Expression-bodied methods": "When the body is a single expression,
// `=> expression` replaces the block." ... "exactly as a `return` operand
// does (§21.7)." ⇒ `=> e` and `return e;` are the same method; their callers
// cannot (and do not) tell them apart.
//
// §28.9 "Runtime-managed types", function-typed values: "the type's lifetime
// is managed by the runtime without user involvement." ⇒ a closure stored in
// a field lives while its holder does; reading it out through a getter must
// not reclaim it.
//
// THE SITE. The caller classifies EVERY call to a user method whose declared
// return type is a function type as a +1 producer
// (`isUserMethodClosureReturnCall`, src/codegen/LlvmBackend_Setup.cpp), so
// `var g := h.Get();` arms a scope-end `bz_closure_release`. The ReturnStmt
// arm (src/codegen/LlvmBackend_Stmts.cpp, "#429 (closure RETURN category)")
// mints that +1 for a borrowed `return this.Fn;`. The arrow-body faces —
// `emitMethodBody`'s `MethodBodyKind::Expression` arm, and the generic
// method's inlined / standalone specialisation faces
// (src/codegen/LlvmBackend_Calls2.cpp) — had the string mint (or none) and
// NO closure mint, so `=> this.Fn` handed back +0 and the caller's release
// dropped the field's only reference.
//
// THE DEFECT, MEASURED on the parent (`bezelc --sanitize=address`, 100x):
//   z07c  `Get() => this.Fn`, call-only          -> heap-use-after-free
//   q3c   `First() => this.Items[0]`             -> heap-use-after-free
//   q4c   `Get() => this.Fn` (owned-Items holder) -> heap-use-after-free
//   q5c   `virtual Get() => this.Fn`             -> heap-use-after-free
//   gen   `GetG<T>(t: T) => this.Fn`             -> heap-use-after-free
//   vgen  `virtual GetG<T>` + arrow override     -> heap-use-after-free
//   coal  `return Pick() ?? throw …` (block)      -> LEAK, one closure per call
//         (`=> Pick() ?? throw …` was clean there; row 11 keeps both clean:
//         a closure `??` of +1 producers is a transfer, not a borrow)
//   w07f  block `return this.Fn;`                -> 1700, clean
//   w07i  property `G => this.Fn`                -> 1700, clean
//
// ── THE ORACLES ────────────────────────────────────────────────────────────
// Every row uses the CHECKED harness (generated-code ASan always on, whatever
// BEZEL_AOT_FORCE_SANITIZE says), so the UAF is a sanitizer failure and
// `r.ran` is false. The toolchain skip is ASSERTED, never absorbed. Each
// program loops 100x so a single early free cannot hide in allocator noise,
// and asserts the exact stdout total, which reads every closure it calls.
//
// `runAotChecked` (LSan on) for the rows whose programs are leak-clean with
// the fix. `runAotCheckedLenient` (LSan off, ASan on) for q3c / q4c / q5c:
// their holder also keeps an `owned Items: Array<(int)->int>` and leaks 72
// bytes in 2 allocations ONCE per process — MEASURED identical for the
// BLOCK-bodied twin of each (`return this.Fn;` / `return this.Items[0];`),
// which this fix does not touch, so that leak is not this defect and these
// rows make no leak claim. Their UAF is still an ASan failure.

#include <cstdio>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::checkedToolchainSkipped;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

void dumpOnFailure(const JitResult& r) {
    if (r.ran) return;
    for (const auto& d : r.diagnostics.diagnostics())
        std::fprintf(stderr, "DIAG [%d]: %s\n", (int)d.severity,
                     d.message.c_str());
}

// ASan + LSan: no UAF, no leak, exact output.
void expectCleanRun(const char* source, const char* expectedStdout) {
    JitResult r = runAotChecked(source);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    dumpOnFailure(r);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(expectedStdout));
}

// ASan only (see the header: a pre-existing, fix-independent 72-byte leak in
// the owned-Items holder): no UAF, exact output.
void expectNoUafRun(const char* source, const char* expectedStdout) {
    JitResult r = runAotCheckedLenient(source);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    dumpOnFailure(r);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(expectedStdout));
}

// ── the programs ───────────────────────────────────────────────────────────

// z07c — THE ISSUE'S REPRO. 100 x (1 + g(1) = 9, h.Fn(1) = 8) = 1700.
const char* kZ07c = R"BZL(module M;
class Holder
  public Fn: (int)->int;
  public constructor() this.Fn := lambda(x: int) => x + 7; end constructor;
  public method Get(): (int)->int => this.Fn;
end Holder;
class P
  public static method Run(h: Holder): int
    var g: (int)->int := h.Get();
    return 1 + g(1);
  end Run;
  public static method Main(): int
    using var h := new Holder();
    var i: int := 0;
    var total: int := 0;
    while i < 100 do total := total + P.Run(h) + h.Fn(1); i := i + 1; end;
    Console.WriteLine(total.ToString());
    return 0;
  end Main;
end P;
)BZL";

// q3c — an ELEMENT read as the arrow body. 100 x (10 + 8 + 10) = 2800.
const char* kQ3c = R"BZL(module M;
class Holder
  public Fn: (int)->int;
  public owned Items: Array<(int)->int>;
  public constructor() this.Fn := lambda(x: int) => x + 7; this.Items := new Array<(int)->int>(); this.Items.Add(lambda(x: int) => x + 9); end constructor;
  public method First(): (int)->int => this.Items[0];
end Holder;
class P
  public static method Run(h: Holder): int
    var g: (int)->int := h.First();
    return g(1);
  end Run;
  public static method Main(): int
    using var h := new Holder();
    var i: int := 0;
    var total: int := 0;
    while i < 100 do total := total + P.Run(h) + h.Fn(1) + h.Items[0](1); i := i + 1; end;
    Console.WriteLine(total.ToString());
    return 0;
  end Main;
end P;
)BZL";

// q4c — the field read in the owned-Items holder. 100 x (8 + 8 + 10) = 2600.
const char* kQ4c = R"BZL(module M;
class Holder
  public Fn: (int)->int;
  public owned Items: Array<(int)->int>;
  public constructor() this.Fn := lambda(x: int) => x + 7; this.Items := new Array<(int)->int>(); this.Items.Add(lambda(x: int) => x + 9); end constructor;
  public method Get(): (int)->int => this.Fn;
end Holder;
class P
  public static method Run(h: Holder): int
    var g: (int)->int := h.Get();
    return g(1);
  end Run;
  public static method Main(): int
    using var h := new Holder();
    var i: int := 0;
    var total: int := 0;
    while i < 100 do total := total + P.Run(h) + h.Fn(1) + h.Items[0](1); i := i + 1; end;
    Console.WriteLine(total.ToString());
    return 0;
  end Main;
end P;
)BZL";

// q5c — a VIRTUAL arrow-bodied method. 100 x (8 + 8 + 10) = 2600.
const char* kQ5c = R"BZL(module M;
class Holder
  public Fn: (int)->int;
  public owned Items: Array<(int)->int>;
  public constructor() this.Fn := lambda(x: int) => x + 7; this.Items := new Array<(int)->int>(); this.Items.Add(lambda(x: int) => x + 9); end constructor;
  public virtual method First(): (int)->int => this.Fn;
end Holder;
class P
  public static method Run(h: Holder): int
    var g: (int)->int := h.First();
    return g(1);
  end Run;
  public static method Main(): int
    using var h := new Holder();
    var i: int := 0;
    var total: int := 0;
    while i < 100 do total := total + P.Run(h) + h.Fn(1) + h.Items[0](1); i := i + 1; end;
    Console.WriteLine(total.ToString());
    return 0;
  end Main;
end P;
)BZL";

// GENERIC arrow-bodied method — inlined at the call site, never through a
// `return` statement (the second face). 100 x (8 + 8 + 9) = 2500.
const char* kGenericArrow = R"BZL(module M;
class Holder
  public Fn: (int)->int;
  public Fn2: (int)->int;
  public constructor() this.Fn := lambda(x: int) => x + 7; this.Fn2 := lambda(x: int) => x + 8; end constructor;
  public method GetG<T>(t: T): (int)->int => this.Fn;
end Holder;
class P
  public static method Run(h: Holder): int
    var g: (int)->int := h.GetG<string>("a");
    return g(1);
  end Run;
  public static method Main(): int
    using var h := new Holder();
    var i: int := 0;
    var total: int := 0;
    while i < 100 do total := total + P.Run(h) + h.Fn(1) + h.Fn2(1); i := i + 1; end;
    Console.WriteLine(total.ToString());
    return 0;
  end Main;
end P;
)BZL";

// The arrow getter's result STORED into another field — a second consumer
// of the same +1 (the store's own release of the displaced value).
// 100 x (Fn2 now Fn: 8, 8, 8) = 2400.
const char* kStoreArrow = R"BZL(module M;
class Holder
  public Fn: (int)->int;
  public Fn2: (int)->int;
  public constructor() this.Fn := lambda(x: int) => x + 7; this.Fn2 := lambda(x: int) => x + 8; end constructor;
  public method Get(): (int)->int => this.Fn;
  public method Swap() this.Fn2 := this.Get(); end Swap;
end Holder;
class P
  public static method Run(h: Holder): int
    h.Swap();
    return h.Fn2(1);
  end Run;
  public static method Main(): int
    using var h := new Holder();
    var i: int := 0;
    var total: int := 0;
    while i < 100 do total := total + P.Run(h) + h.Fn(1) + h.Fn2(1); i := i + 1; end;
    Console.WriteLine(total.ToString());
    return 0;
  end Main;
end P;
)BZL";

// CONTROL — the BLOCK-bodied twin of z07c (`return this.Fn;`). 1700 clean on
// both arms: the reference behaviour the arrow form must reproduce.
const char* kBlockControl = R"BZL(module M;
class Holder
  public Fn: (int)->int;
  public constructor() this.Fn := lambda(x: int) => x + 7; end constructor;
  public method Get(): (int)->int
    return this.Fn;
  end Get;
end Holder;
class P
  public static method Run(h: Holder): int
    var g: (int)->int := h.Get();
    return 1 + g(1);
  end Run;
  public static method Main(): int
    using var h := new Holder();
    var i: int := 0;
    var total: int := 0;
    while i < 100 do total := total + P.Run(h) + h.Fn(1); i := i + 1; end;
    Console.WriteLine(total.ToString());
    return 0;
  end Main;
end P;
)BZL";

// CONTROL — the PROPERTY getter `G => this.Fn`. A property read is a lend
// (§28.9 "Accessor faces are uniform across type kinds": "a getter never
// transfers a reclamation obligation to its reader"), so the fix must NOT
// mint here (axiom A4). 1700 clean on both arms; a mint would leak the cell.
const char* kPropertyControl = R"BZL(module M;
class Holder
  public Fn: (int)->int;
  public constructor() this.Fn := lambda(x: int) => x + 7; end constructor;
  public property G: (int)->int => this.Fn;
end Holder;
class P
  public static method Run(h: Holder): int
    var g: (int)->int := h.G;
    return 1 + g(1);
  end Run;
  public static method Main(): int
    using var h := new Holder();
    var i: int := 0;
    var total: int := 0;
    while i < 100 do total := total + P.Run(h) + h.Fn(1); i := i + 1; end;
    Console.WriteLine(total.ToString());
    return 0;
  end Main;
end P;
)BZL";

// CONTROL — an arrow body that is itself a +1 PRODUCER (a lambda literal):
// the transfer exclusion must hold, so no second +1 is minted (a mint would
// leak one 64-byte cell per call — 100 here). 100 x (4 + 8 + 9) = 2100.
const char* kProducerControl = R"BZL(module M;
class Holder
  public Fn: (int)->int;
  public Fn2: (int)->int;
  public constructor() this.Fn := lambda(x: int) => x + 7; this.Fn2 := lambda(x: int) => x + 8; end constructor;
  public method Mk(): (int)->int => lambda(x: int) => x + 3;
end Holder;
class P
  public static method Run(h: Holder): int
    var g: (int)->int := h.Mk();
    return g(1);
  end Run;
  public static method Main(): int
    using var h := new Holder();
    var i: int := 0;
    var total: int := 0;
    while i < 100 do total := total + P.Run(h) + h.Fn(1) + h.Fn2(1); i := i + 1; end;
    Console.WriteLine(total.ToString());
    return 0;
  end Main;
end P;
)BZL";

// A VIRTUAL GENERIC arrow-bodied getter with an arrow-bodied override —
// dispatched by descriptor to a standalone per-(class, type-arg)
// specialisation (the third face, `Holder$GetG$spec` / `Sub$GetG$spec`).
// 100 x (8 + 9 + 8 + 9) = 3400.
const char* kVirtualGenericArrow = R"BZL(module M;
class Holder
  public Fn: (int)->int;
  public Fn2: (int)->int;
  public constructor() this.Fn := lambda(x: int) => x + 7; this.Fn2 := lambda(x: int) => x + 8; end constructor;
  public virtual method GetG<T>(t: T): (int)->int => this.Fn;
end Holder;
class Sub: Holder
  public constructor() base(); end constructor;
  public override method GetG<T>(t: T): (int)->int => this.Fn2;
end Sub;
class P
  public static method Run(h: Holder): int
    var g: (int)->int := h.GetG<string>("a");
    return g(1);
  end Run;
  public static method Main(): int
    using var h := new Holder();
    using var s := new Sub();
    var i: int := 0;
    var total: int := 0;
    while i < 100 do total := total + P.Run(h) + P.Run(s) + h.Fn(1) + s.Fn2(1); i := i + 1; end;
    Console.WriteLine(total.ToString());
    return 0;
  end Main;
end P;
)BZL";

// `Pick(…) ?? throw …` returned from an arrow body AND from its block twin.
// A closure `??` is not normalized by lowerCoalesce, so the result is the
// producer arm's own +1: a TRANSFER the return-site mint must not add to
// (the shape `function_value_at_object_spec_12_7_5978`'s `Rx` pins clean).
// 100 x (2 + 4) = 600.
const char* kCoalesceTransfer = R"BZL(module M;
class Program
  public static method Pick(some: bool, k: int): ((int) -> int)?
    if some then return lambda(x: int) => x * 2 + k - k; end;
    return null;
  end Pick;
  public static method Rx(i: int): (int) -> int =>
    Program.Pick(true, i) ?? throw new InvalidOperationException("x");
  public static method Rb(i: int): (int) -> int
    return Program.Pick(true, i) ?? throw new InvalidOperationException("x");
  end Rb;
  public static method Main(): int
    var t := 0;
    var i := 0;
    while i < 100 do
      var rx := Program.Rx(i);
      var rb := Program.Rb(i);
      t := t + rx(1) + rb(2);
      i := i + 1;
    end;
    Console.WriteLine(t.ToString());
    return 0;
  end Main;
end Program;
)BZL";

// ── fix-forward (review r39-rev-8141, B1): the CHAIN-call site ─────────────
// `h.Get()(1)` re-derives "did the callee's return mint?" from the callee's
// AST (`innerMints`, src/codegen/LlvmBackend_Calls2.cpp, fix #3704/#4239) and
// releases twice when it thinks so. It must list the same transfer exclusions
// as the return-site mint gate, `isCoalescedClosureTransfer` included, or a
// `??` transfer is released twice: "refcount over-release (closure)".
// MEASURED on 09d18434: c01 / c02 / c03 abort rc=134; on 301f24f2 c01 printed
// 200 (c02 / c03 were already rc=134 there). 100 x (2 * 1) = 200.
const char* kChainCoalesceBlock = R"BZL(module M;
class Holder
  public static method Pick(some: bool): ((int)->int)?
    if some then return lambda(x: int) => x * 2; end;
    return null;
  end Pick;
  public method Get(): (int)->int
    return Holder.Pick(true) ?? throw new InvalidOperationException("x");
  end Get;
end Holder;
class P
  public static method Main(): int
    using var h := new Holder();
    var i: int := 0;
    var total: int := 0;
    while i < 100 do total := total + h.Get()(1); i := i + 1; end;
    Console.WriteLine(total.ToString());
    return 0;
  end Main;
end P;
)BZL";

const char* kChainCoalesceArrow = R"BZL(module M;
class Holder
  public static method Pick(some: bool): ((int)->int)?
    if some then return lambda(x: int) => x * 2; end;
    return null;
  end Pick;
  public method Get(): (int)->int => Holder.Pick(true) ?? throw new InvalidOperationException("x");
end Holder;
class P
  public static method Main(): int
    using var h := new Holder();
    var i: int := 0;
    var total: int := 0;
    while i < 100 do total := total + h.Get()(1); i := i + 1; end;
    Console.WriteLine(total.ToString());
    return 0;
  end Main;
end P;
)BZL";

// The arrow `?? lambda` twin: both arms are +1 producers, so the `??` is a
// transfer and the chain site must release once.
const char* kChainCoalesceLambdaArrow = R"BZL(module M;
class Holder
  public static method Pick(some: bool): ((int)->int)?
    if some then return lambda(x: int) => x * 2; end;
    return null;
  end Pick;
  public method Get(): (int)->int => Holder.Pick(true) ?? lambda(x: int) => x * 3;
end Holder;
class P
  public static method Main(): int
    using var h := new Holder();
    var i: int := 0;
    var total: int := 0;
    while i < 100 do total := total + h.Get()(1); i := i + 1; end;
    Console.WriteLine(total.ToString());
    return 0;
  end Main;
end P;
)BZL";

}  // namespace

// ===========================================================================
// ROW 1 — z07c, the issue's repro. RED before the fix (heap-use-after-free).
// ===========================================================================
BEZEL_TEST(ownership_expr_body_getter_closure_cell_8141,
           spec_22_2_arrow_getter_of_field_closure_does_not_over_release) {
    expectCleanRun(kZ07c, "1700\n");
}

// ===========================================================================
// ROW 2 — q3c, `=> this.Items[0]`. RED before the fix (heap-use-after-free).
// ===========================================================================
BEZEL_TEST(ownership_expr_body_getter_closure_cell_8141,
           spec_22_2_arrow_getter_of_element_closure_does_not_over_release) {
    expectNoUafRun(kQ3c, "2800\n");
}

// ===========================================================================
// ROW 3 — q4c, `=> this.Fn` in the owned-Items holder. RED before the fix.
// ===========================================================================
BEZEL_TEST(ownership_expr_body_getter_closure_cell_8141,
           spec_22_2_arrow_getter_in_owned_items_holder_does_not_over_release) {
    expectNoUafRun(kQ4c, "2600\n");
}

// ===========================================================================
// ROW 4 — q5c, a `virtual` arrow-bodied getter. RED before the fix.
// ===========================================================================
BEZEL_TEST(ownership_expr_body_getter_closure_cell_8141,
           spec_22_2_virtual_arrow_getter_does_not_over_release) {
    expectNoUafRun(kQ5c, "2600\n");
}

// ===========================================================================
// ROW 5 — a GENERIC arrow-bodied getter (the inlined specialisation face).
// RED before the fix (heap-use-after-free).
// ===========================================================================
BEZEL_TEST(ownership_expr_body_getter_closure_cell_8141,
           spec_22_2_generic_arrow_getter_does_not_over_release) {
    expectCleanRun(kGenericArrow, "2500\n");
}

// ===========================================================================
// ROW 6 — the arrow getter's result stored into a field. RED before the fix.
// ===========================================================================
BEZEL_TEST(ownership_expr_body_getter_closure_cell_8141,
           spec_22_2_arrow_getter_result_stored_to_field_does_not_over_release) {
    expectCleanRun(kStoreArrow, "2400\n");
}

// ===========================================================================
// ROW 7 — CONTROL: the block-bodied twin. Green on both arms.
// ===========================================================================
BEZEL_TEST(ownership_expr_body_getter_closure_cell_8141,
           control_block_body_return_of_field_closure_is_clean) {
    expectCleanRun(kBlockControl, "1700\n");
}

// ===========================================================================
// ROW 8 — CONTROL: the property getter (a lend, A4). Green on both arms.
// ===========================================================================
BEZEL_TEST(ownership_expr_body_getter_closure_cell_8141,
           control_property_getter_of_field_closure_is_clean) {
    expectCleanRun(kPropertyControl, "1700\n");
}

// ===========================================================================
// ROW 9 — CONTROL: a producer arrow body transfers, it is not re-minted.
// Green on both arms.
// ===========================================================================
BEZEL_TEST(ownership_expr_body_getter_closure_cell_8141,
           control_producer_arrow_body_transfers_without_a_second_mint) {
    expectCleanRun(kProducerControl, "2100\n");
}

// ===========================================================================
// ROW 10 — a VIRTUAL GENERIC arrow getter + arrow override (the standalone
// specialisation face). RED before the fix.
// ===========================================================================
BEZEL_TEST(ownership_expr_body_getter_closure_cell_8141,
           spec_22_2_virtual_generic_arrow_getter_does_not_over_release) {
    expectCleanRun(kVirtualGenericArrow, "3400\n");
}

// ===========================================================================
// ROW 11 — `=> Pick() ?? throw` and `return Pick() ?? throw`: both transfer
// the producer's +1. RED on the parent (the BLOCK twin leaks one closure
// per call there: it minted on top of the transfer); the arrow half went
// RED with the first cut of this fix, before isCoalescedClosureTransfer.
// ===========================================================================
BEZEL_TEST(ownership_expr_body_getter_closure_cell_8141,
           spec_28_9_coalesced_producer_return_transfers_in_both_body_forms) {
    expectCleanRun(kCoalesceTransfer, "600\n");
}

// ===========================================================================
// ROW 12 — c01, block `return Pick() ?? throw` called as `h.Get()(1)`.
// GREEN on 301f24f2, RED on 09d18434 (chain-site double release).
// ===========================================================================
BEZEL_TEST(ownership_expr_body_getter_closure_cell_8141,
           spec_28_9_chain_call_of_block_coalesced_transfer_releases_once) {
    expectCleanRun(kChainCoalesceBlock, "200\n");
}

// ===========================================================================
// ROW 13 — c02, the arrow twin `=> Pick() ?? throw`, chain-called.
// ===========================================================================
BEZEL_TEST(ownership_expr_body_getter_closure_cell_8141,
           spec_22_2_chain_call_of_arrow_coalesced_transfer_releases_once) {
    expectCleanRun(kChainCoalesceArrow, "200\n");
}

// ===========================================================================
// ROW 14 — arrow `=> Pick() ?? lambda`, chain-called.
// ===========================================================================
BEZEL_TEST(ownership_expr_body_getter_closure_cell_8141,
           spec_22_2_chain_call_of_arrow_coalesced_lambda_releases_once) {
    expectCleanRun(kChainCoalesceLambdaArrow, "200\n");
}
