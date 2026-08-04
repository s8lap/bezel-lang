// Codegen — issue #2731 item 3: `pendingOwnedParams_` / `pendingArgsDisposal_`
// were not covered by `Emitter::NestedBodyScope` (#2702), so a lazily-emitted,
// MEMOIZED nested body lowered from inside a `requires` prologue STOLE the
// enclosing method's pending `owned`-parameter registrations. The parameter is
// then never disposed — a §28.3 auto-drop the compiler has already promised in
// a diagnostic and then does not emit.
//
// The issue filed this item as "plausible, reachability UNPROVEN", turning on
// whether sema admits a `requires` clause that reads a block-bodied property.
// It does — verified below — and the leak is real.
//
// Spec (revision v0.240):
//   §22.16 "Contract clauses" — a clause expression "is admitted under the
//     **pure-body rules of §22.15**, applied expression-wise". So a clause MAY
//     read an accessor, but only a pure one: §22.15 admits an accessor read iff
//     the member is `pure`-declared or pure-classified, and pure-classification
//     covers compiler-synthesized bodies only — "Authored bodies always require
//     the explicit keyword." An authored `get => do … end` with no marker is
//     therefore INADMISSIBLE in a clause. ⚠ It compiles today anyway, because
//     `src/sema/PureChecker.cpp:202` returns `true` for every `PropertySymbol*`
//     with a comment saying pure-classification "lands later". The accessors
//     below are marked `pure` so these cases rest on the spec rather than on
//     that deferred approximation, and do not turn into compile failures the
//     moment the classification slice lands. §22.15: "Expression-bodied and
//     read-only block-form properties take property-level `pure` … indexers
//     follow the same forms."
//   A `pure` block getter is still an AUTHORED BLOCK BODY, so it is still
//     lowered by `ensurePropertyBlockGetter` into its own memoized function —
//     the marker changes admissibility, not the emission path, and the defect
//     reproduces identically with it.
//   §23.3 "Block-form properties" — "Each accessor uses `=> expression` or
//     `=> do ... end`." The `do ... end` form is lowered into its OWN memoized
//     function, which is what makes the read a NESTED BODY EMISSION.
//   §28.3 "Local variables, ownership, and borrows" — on an undischarged
//     `owned` parameter the compiler emits a must-use WARNING at the implicit
//     drop site, and "The warning is a lint over a sound program — it changes
//     no semantics, only flags the implicit drop — so a project may tune or
//     silence it; **the auto-drop runs regardless**." The defect is exactly
//     that the auto-drop does NOT run.
//
// THE DEFECT, precisely.
//   * `bindParameters` (LlvmBackend_Setup.cpp ~8579) fills `pendingOwnedParams_`
//     with the method's `owned` parameters BEFORE any scope exists.
//   * The FIRST root-scope `lowerScopedBlock` (Setup.cpp ~21701, gated on
//     `disposeStack_.size() == 1`) moves the set out and registers each entry
//     for §28.3 auto-drop.
//   * `emitRequiresPrologue` lowers the clause's user expression (Setup.cpp
//     ~4556) in BETWEEN — after `bindParameters`, before the body's own
//     `lowerScopedBlock` (~6348). A block-bodied property read in the clause
//     emits its getter there, and every emitter CLEARS `disposeStack_` first,
//     so the getter's own block IS a root scope and consumes the caller's set.
//   * The consumption is SILENT rather than a cross-function IR reference:
//     `registerUnwindOwnedParam` (Setup.cpp ~11916) bails on
//     `currentFn_ != unwindIneligibleFn_`, so the stolen registration is
//     dropped on the floor, and the enclosing method's own `lowerScopedBlock`
//     later finds the set already empty.
// Setup.cpp ~21699 states the invariant this breaks in so many words: "a
// lambda/thunk body that opens its own root scope inherits an empty
// pendingOwnedParams_ unless its own bindParameters filled it."
//
// THE FIX. Both members join `NestedBodyScope` — saved, cleared at entry,
// restored on exit — so a nested body can only ever consume registrations its
// OWN parameter binding produced.
//
// THE ORACLE. `Res.n` is the live count and `Res.made` the construction count.
// `Take` returns `Res.n * 100 + Res.made` (evaluated INSIDE the method, before
// the auto-drop), and `Main` adds `Res.n` read AFTER the call. Correct is
// 101 + 0 = 101; the leak shows up as 101 + 1 = 102, a value, not a crash.
// ⚠ These cases are deliberately NOT written against a sanitizer verdict: the
// leaked object is reachable from nothing at exit, but LSan's verdict on this
// shape is exactly the kind the issue warns can be layout dependent, so the
// counter is the primary oracle and the checked run is a second opinion.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

bool wasSkipped(const JitResult& r) {
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.message.find("SKIP") != std::string::npos) return true;
    }
    return false;
}

int runPlain(const std::string& src) {
    JitResult r = bezel::test::runAot(src);
    return r.ran ? r.exitCode : -999;
}

int runChecked(const std::string& src) {
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return -1000;   // ASan toolchain absent — case skipped
    return r.ran ? r.exitCode : -999;
}

// `accessor` is the property spelling under test; `clause` is the `requires`
// expression. Everything else is held fixed across all four cases so the ONLY
// difference between the repro and its controls is what the clause reads.
std::string program(const std::string& accessor, const std::string& clause) {
    return std::string("module M; ")
        + "class Res "
          "  public static n: int := 0; "
          "  public static made: int := 0; "
          "  public constructor() Res.n := Res.n + 1; "
          "    Res.made := Res.made + 1; end constructor; "
          "  destructor Res.n := Res.n - 1; end destructor; "
          "end Res; "
          "class Owner "
          "  private _n: int; "
          "  public constructor() _n := 7; end constructor; "
        + accessor
        + "  public method Take(owned r: Res): int "
          "    requires(" + clause + ") "
          "    return Res.n * 100 + Res.made; "   // inside: 1 alive, 1 made
          "  end Take; "
          "end Owner; "
          "class C "
          "  public static method Main(): int "
          "    using var o := new Owner(); "
          "    var k := o.Take(new Res()); "
          "    return k + Res.n; "                // + 0 once `r` is dropped
          "  end Main; "
          "end C;";
}

// `pure` per §22.15 — required for the clause to be admissible at all (see the
// header note). The body is still an authored `do … end`, so the emission path
// is unchanged.
const char* kBlockProperty =
    "  public pure property N: int "
    "    get => do "
    "      return _n; "
    "    end; "
    "  end N; ";

const char* kExprProperty =
    "  public pure property N: int "
    "    get => _n; "
    "  end N; ";

}  // namespace

// ── (1) THE REPRO: a `requires` clause that reads a BLOCK-bodied property, on a
// method with an `owned` parameter. Pre-fix 102 (the `Res` is never disposed);
// post-fix 101. That the program COMPILES AND RUNS at all is itself the answer
// to the issue's open question — this shape parses and typechecks, so item 3 is
// not vacuous.
static const std::string kRequiresReadsBlockProperty =
    program(kBlockProperty, "N > 0");

BEZEL_TEST(codegen_requires_nested_body_2731,
           owned_param_dropped_when_requires_reads_block_property) {
    BEZEL_EXPECT_EQ(runPlain(kRequiresReadsBlockProperty), 101);
}

BEZEL_TEST(codegen_requires_nested_body_2731,
           owned_param_dropped_when_requires_reads_block_property_checked) {
    int rc = runChecked(kRequiresReadsBlockProperty);
    if (rc == -1000) return;   // ASan toolchain absent
    BEZEL_EXPECT_EQ(rc, 101);
}

// ── (2) OVER-CORRECTION CONTROL A: the identical program whose clause reads an
// EXPRESSION-bodied property (§23.3's other accessor form). An expression getter
// is inlined at the read site — no nested body, no root scope, nothing stolen —
// so this passes BOTH pre-fix and post-fix. It is what proves the defect is the
// nested emission and not `requires`-plus-`owned`-parameter in general.
static const std::string kRequiresReadsExprProperty =
    program(kExprProperty, "N > 0");

BEZEL_TEST(codegen_requires_nested_body_2731, expression_property_control) {
    BEZEL_EXPECT_EQ(runPlain(kRequiresReadsExprProperty), 101);
}

// ── (3) OVER-CORRECTION CONTROL B: the clause reads a plain FIELD, so no
// accessor of any kind is involved. Passes both pre- and post-fix; pins that the
// `owned`-parameter auto-drop itself was never broken.
static const std::string kRequiresReadsField = program(kBlockProperty, "_n > 0");

BEZEL_TEST(codegen_requires_nested_body_2731, plain_field_clause_control) {
    BEZEL_EXPECT_EQ(runPlain(kRequiresReadsField), 101);
}

// ── (4) The block-bodied INDEXER spelling (§23.4) of case (1), through the
// guard's second adopter `ensureIndexerBlockGetter`. Same window, same fix.
static const std::string kRequiresReadsBlockIndexer = program(
    "  public pure property [i: int]: int "
    "    get => do "
    "      return _n + i; "
    "    end; "
    "  end; ",
    "this[0] > 0");

BEZEL_TEST(codegen_requires_nested_body_2731,
           owned_param_dropped_when_requires_reads_block_indexer) {
    BEZEL_EXPECT_EQ(runPlain(kRequiresReadsBlockIndexer), 101);
}

// ── (5) A SECOND, PURITY-INDEPENDENT ROUTE INTO THE SAME WINDOW: a CONSTRUCTOR
// FIELD INITIALIZER. `emitOwnFieldInit` (LlvmBackend_Setup.cpp ~7454) lowers
// field initializers after `bindParameters` (~7287) and before the constructor
// body's own `lowerScopedBlock` (~7456) — the identical gap the `requires`
// prologue sits in — and a field initializer carries NO purity restriction, so
// this case does not depend on §22.15/§22.16 admissibility or on the deferred
// PureChecker approximation at all. It is the durable half of this regression:
// if the pure-classification slice ever changes what a clause may read, cases
// (1)-(4) move with it and this one does not.
//
// The initializer must read the accessor on a FRESHLY CONSTRUCTED OTHER object:
// §22.8 rejects a field initializer that references an instance member of its
// own class ("a field initializer may not reference instance member `N`"), and
// the static-property spelling hits a separate pre-existing #1386 ICE
// ("internal: lowerExpr returned null for instance field/property initializer").
// `(new Src()).N` is neither, and is admitted.
//
// Oracle: `Owner`'s constructor takes `owned r: Res` and never discharges it, so
// §28.3 auto-drop must dispose it at constructor end — 0 alive, 1 made, and the
// cached getter value 5 flows out to prove the getter actually ran:
// 0 * 100 + 1 + 5 = 6. Pre-fix the parameter survives: 1 * 100 + 1 + 5 = 106.
static const std::string kCtorFieldInitReadsBlockProperty =
    std::string("module M; ")
    + "class Res "
      "  public static n: int := 0; "
      "  public static made: int := 0; "
      "  public constructor() Res.n := Res.n + 1; "
      "    Res.made := Res.made + 1; end constructor; "
      "  destructor Res.n := Res.n - 1; end destructor; "
      "end Res; "
      "class Src "
      "  private _v: int; "
      "  public constructor() _v := 5; end constructor; "
      "  public property N: int "         // no `pure` needed here — not a clause
      "    get => do "
      "      return _v; "
      "    end; "
      "  end N; "
      "end Src; "
      "class Owner "
      "  private _cache: int := (new Src()).N; "
      "  public constructor(owned r: Res) "
      "  end constructor; "
      "  public method Cache(): int "
      "    return _cache; "
      "  end Cache; "
      "end Owner; "
      "class C "
      "  public static method Main(): int "
      "    var c := 0; "
      "    do "
      "      using var o := new Owner(new Res()); "
      "      c := o.Cache(); "
      "    end; "
      "    return Res.n * 100 + Res.made + c; "   // 0 + 1 + 5 = 6
      "  end Main; "
      "end C;";

// ⚠ PLAIN ORACLE ONLY, deliberately. This case has no `runAotChecked` twin
// because the `(new Src())` unbound owned temporary in a field-initializer
// position is never disposed — a SEPARATE, PRE-EXISTING leak (#2751), isolated
// here: it reproduces identically with an EXPRESSION-bodied `Src.N` (no nested
// body emission at all) and with a constructor that has NO `owned` parameter,
// so it is independent of both this defect and its fix. Asserting the checked
// oracle here would assert on that unrelated leak. The `Res` counter folded
// into the exit code is unaffected by it and remains the oracle for THIS
// defect; cases (1)-(3) carry the checked coverage.
BEZEL_TEST(codegen_requires_nested_body_2731,
           owned_ctor_param_dropped_when_field_init_reads_block_property) {
    BEZEL_EXPECT_EQ(runPlain(kCtorFieldInitReadsBlockProperty), 6);
}
