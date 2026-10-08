// Spec-derived regression tests for issue #3769 — a constructor call that
// OMITS a §22.5-defaulted parameter matched no formal list by exact arity, so
// an `owned` parameter sitting BEFORE the omitted default was not an owned
// sink: the argument was neither §22.3 marker-checked nor §28.8 marked
// moved-from, and the §28.3 auto-drop then freed a value the callee had already
// transferred into an `owned` field.
//
// ── THE SHAPE (issue 3769's repro, verbatim) ───────────────────────────────
//
//     class Base(owned r: Res, tag: int := 5)
//       private owned _s: Res := r;
//       private _t: int := tag;
//       public method Show(): string => $"{_t}/{_s.Id()}";
//     end Base;
//
//     var x := new Res(6);
//     using var b := new Base(x);          // (a) NO diagnostic at all
//     Console.WriteLine(b.Show());         //     heap-use-after-free
//
// and its `this(...)` twin:
//
//     public constructor(owned x: Res, flag: bool)
//       this(x);                           // (b) auto-drop WARNING only
//     end constructor;                     //     heap-use-after-free
//
// ── WHAT THE DEFECT WAS (measured on r12-cycle-1 @ 89eb914) ────────────────
//
// `ownership::resolveCtorFormals` matched a constructor call to its formals by
// EXACT arity: `sema::formalParamCount(*cs) != nArgs` skipped every declared
// candidate and the §22.10 primary arm required
// `td->positionalFields.size() == nArgs`. A call omitting a defaulted trailing
// parameter therefore matched NOTHING, the `CalleeFormals` view was invalid,
// and every §22.3 / §28.8 consumer read that as "this argument position is not
// an owned sink". Both halves above were `AddressSanitizer:
// heap-use-after-free`; spelling the default explicitly (`new Base(x, 5)`,
// `this(owned x, 5)`) made the arity exact and both were classified correctly,
// which is what pinned the arity match as the whole defect.
//
// Conversely there was no CORRECT spelling of the transfer: `new Base(owned x)`
// was rejected with "this parameter position does not take ownership" — naming
// a position that IS `owned`. R2 / R4 / R6 / R8 / R13 pin that those now work.
//
// ── WHY THE ADMISSION RULE IS A PARAMETER, NOT ONE RULE ────────────────────
//
// The fix resolves formals by the rule the LOWERING resolvers use — but the two
// lowering resolvers do not agree with each other, and adopting the wrong one
// is a fresh soundness hole in the opposite direction:
//
//   * a `new T(args)` site lowers through codegen's `ctorForArgs`: exact arity,
//     then the first explicit constructor whose formals the arguments CAN FILL
//     with §22.5 defaults admitted (`argsCanFill`, fix #3082), then the §22.10
//     primary. R7 / R8 pin the explicit-fill arm and R9 pins that exact arity
//     still wins first (§22.6's "Defaulted-arity tie-break").
//   * a `this(...)` / `base(...)` chain lowers through `emitChainCall`. When
//     #3769 landed that resolver used `ctorByArity` — EXACT arity, no defaults
//     — so a `this(x)` chain to an explicit sibling across an omitted default
//     lowered to NOTHING (`class B` with `constructor(a: int, b: int := 3)`
//     printed `0/0`), and R11 pinned ownership NOT classifying that argument:
//     declaring a transfer into a constructor that is never called would have
//     suppressed the §28.3 auto-drop and LEAKED.
//
//     fix #3777 (the commit that owns this file's R11 today) made that site
//     select with `ctorForArgs` and materialise the omitted §22.5 tail, so the
//     sibling constructor now really runs — which inverts the carve-out into a
//     DOUBLE FREE. R11 is therefore REVERSED: the unmarked spelling is now a
//     §22.3 REJECT and `this(owned x)` is the accepted one, disposing exactly
//     once. `CtorSelect::ChainSite` still differs from `NewSite` in one place
//     only — a NAMED short chain call, which `emitChainCall` still lowers by
//     the exact-arity rule.
//
// ── ROWS THAT PASS ON BOTH SIDES, BY DESIGN ────────────────────────────────
//
// R9 and R10 are anti-over-reject pins: they assert behaviour this fix must NOT
// change (the §22.6 exact-arity preference, and a plain omitted-default
// positional staying a non-sink with its default still materialized). A fix
// that widened past its warrant breaks them. R11 WAS the third such pin (the
// chain-site carve-out); fix #3777 removed the lowering gap it was built
// around, so it is now a REJECT row with an ACCEPT twin — see above.

#include <string>

#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// `Res` carries BOTH static counters. `made` alone cannot separate "constructed
// once and disposed twice" from "constructed twice"; `live` alone cannot see a
// value that was never made. The pair is the oracle for every ×200 row.
const char* kRes =
    "class Res "
    "  public static _live: int := 0; "
    "  public static _made: int := 0; "
    "  private _id: int; "
    "  public constructor(id: int) "
    "    _id := id; "
    "    _live := _live + 1; "
    "    _made := _made + 1; "
    "  end constructor; "
    "  destructor "
    "    _live := _live - 1; "
    "  end destructor; "
    "  public method Id(): int => _id; "
    "end Res; ";

// A destructor-only `Res` for the diagnostic-only rows: no static counters, so
// the §22.12 rule-2 static warnings do not appear beside the assertion.
const char* kResPlain =
    "class Res "
    "  destructor end destructor; "
    "  public method Id(): int => 1; "
    "end Res; ";

// The §22.10 primary constructor of the issue's repro: an `owned` parameter
// sitting BEFORE a §22.5-defaulted one. Omitting `tag` is the whole defect.
const char* kBasePrimaryOwnedThenDefault =
    "class Base(owned r: Res, tag: int := 5) "
    "  private owned _s: Res := r; "
    "  private _t: int := tag; "
    "  public method Id(): int "
    "    return _t + _s.Id(); "
    "  end Id; "
    "end Base; ";

// The §22.3 (v0.169) missing-owned-marker diagnostic —
// `reportMissingOwnedMarker`. Matched on TEXT plus its section citation, never
// on `hasErrors()`.
bool hasMissingOwnedMarker(const OwnershipResult& r, const std::string& name) {
    const std::string needle = "this call consumes `" + name + "`";
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos &&
            d.message.find("§22.3") != std::string::npos)
            return true;
    return false;
}

// The §22.3 SPURIOUS-marker rejection — "this parameter position does not take
// ownership". Pre-fix this fired on the CORRECT spelling `new Base(owned x)`,
// naming a position that is in fact `owned`; its absence is half of R2/R4's
// proof and its presence is what R9/R11 keep for the positions that really are
// not owned sinks.
bool hasSpuriousOwnedMarkerReject(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("this parameter position does not take ownership") !=
                std::string::npos &&
            d.message.find("§22.3") != std::string::npos)
            return true;
    return false;
}

// The §28.3 auto-drop WARNING on an `owned` parameter. Its presence is the
// pre-fix signature at a chain site: it fires exactly when the parameter was
// still Owned at the end of the convenience constructor — i.e. when the chain
// did NOT consume it, which is when codegen emits the free that the chained-to
// constructor's field store makes a use-after-free.
bool hasOwnedParamAutoDropWarning(const OwnershipResult& r,
                                  const std::string& name) {
    const std::string needle = "owned parameter `" + name + "`";
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos &&
            d.message.find("auto-drop") != std::string::npos)
            return true;
    return false;
}

// The ×200 driver used by every accept row: construct, read the transferred
// field, dispose, 200 times, and print the counter pair plus the accumulated
// field reads. `acc` proves the TRANSFERRED value — not a zeroed slot — reached
// the field; `made`/`live` prove exactly-once construction and disposal.
std::string loop200(const std::string& newExpr) {
    return "class C "
           "  public static method Main(): int "
           "    var i: int := 0; "
           "    var acc: int := 0; "
           "    while i < 200 do "
           "      " + newExpr +
           "      acc := acc + b.Id(); "
           "      i := i + 1; "
           "    end while; "
           "    Console.WriteLine($\"acc={acc} made={Res._made} "
           "live={Res._live}\"); "
           "    return 0; "
           "  end Main; "
           "end C; ";
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// R1 — REJECT. THE HEADLINE REPRO (a): `new Base(x)` where `x` is a named owned
// local and the §22.10 primary's parameter before the omitted default is
// `owned`. §22.3: "Omitting the marker on a named owned binding is a compile
// error naming the fix".
// FAIL-WITHOUT: accepted with NO diagnostic at all, and
// `AddressSanitizer: heap-use-after-free` at run time (measured on 89eb914).
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5 (a call may omit a defaulted trailing parameter) / §22.3 (call-site
// marker required on a named owned binding) / §22.10, issue #3769
BEZEL_TEST(defaulted_ctor_arity_owned_sink_3769,
           new_site_unmarked_owned_local_to_owned_primary_across_omitted_default_rejected) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kResPlain + kBasePrimaryOwnedThenDefault +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(6); "
        "    using var b := new Base(x); "
        "    return b.Id(); "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(hasMissingOwnedMarker(r, "x"));
}

// ═══════════════════════════════════════════════════════════════════════════
// R2 — ACCEPT, ASan/LSan, 200 iterations. The CORRECT spelling of (a).
// FAIL-WITHOUT: rejected with "this parameter position does not take
// ownership" — pre-fix no correct spelling of this transfer existed.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.3 (the `owned` call-site marker) / §28.8 (ownership transfer) /
// §22.5, issue #3769
BEZEL_TEST(defaulted_ctor_arity_owned_sink_3769,
           new_site_owned_marker_to_owned_primary_across_omitted_default_asan_x200) {
    const std::string src =
        std::string("module M3769b; ") + kRes + kBasePrimaryOwnedThenDefault +
        loop200("var x := new Res(i); "
                "using var b := new Base(move x); ");
    auto r = runAotChecked(src);
    // `ran == false` is BOTH the ASan/LSan failure signal and the "cannot build
    // an instrumented executable here" skip — assert it so neither passes
    // silently.
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // acc = sum(i + 5, i in 0..199) = 19900 + 1000. `made=200 live=0` is
    // exactly-once construction and exactly-once disposal; the `+5` is the
    // §22.5 default actually materialized at the omitted position.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("acc=20900 made=200 live=0") !=
                      std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// R3 — REJECT. THE HEADLINE REPRO (b): the `this(...)` chain twin. The chain
// omits the defaulted `tag`, so pre-fix it matched neither the §22.10 primary
// (arity 2) nor the enclosing convenience constructor (excluded by §22.10's
// no-self-chain rule).
// FAIL-WITHOUT: accepted with only the §28.3 auto-drop warning, and
// `AddressSanitizer: heap-use-after-free` at run time (measured on 89eb914).
// ═══════════════════════════════════════════════════════════════════════════
// per §12.8 (constructor chaining) / §22.5 / §22.3 / §28.8, issue #3769
BEZEL_TEST(defaulted_ctor_arity_owned_sink_3769,
           chain_unmarked_owned_param_to_owned_primary_across_omitted_default_rejected) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kResPlain +
        "class Base(owned r: Res, tag: int := 5) "
        "  private owned _s: Res := r; "
        "  private _t: int := tag; "
        "  public constructor(owned x: Res, flag: bool) "
        "    this(x); "
        "  end constructor; "
        "  public method Id(): int "
        "    return _t + _s.Id(); "
        "  end Id; "
        "end Base; ");
    BEZEL_EXPECT_TRUE(hasMissingOwnedMarker(r, "x"));
    // The pre-fix signature must be GONE: with the chain consuming `x`, the
    // parameter is moved-from at the constructor's end, so nothing auto-drops
    // and codegen emits no free.
    BEZEL_EXPECT_FALSE(hasOwnedParamAutoDropWarning(r, "x"));
}

// ═══════════════════════════════════════════════════════════════════════════
// R4 — ACCEPT, ASan/LSan, 200 iterations. The correct spelling of (b), with the
// default still omitted. This is the arm codegen's #3583
// `storePrimaryCtorPositionalArgs` fills, so ownership and lowering agree.
// FAIL-WITHOUT: rejected ("this parameter position does not take ownership").
// ═══════════════════════════════════════════════════════════════════════════
// per §12.8 / §22.10 / §22.3 / §28.8 / §22.5, issue #3769
BEZEL_TEST(defaulted_ctor_arity_owned_sink_3769,
           chain_owned_marker_to_owned_primary_across_omitted_default_asan_x200) {
    const std::string src =
        std::string("module M3769d; ") + kRes +
        "class Base(owned r: Res, tag: int := 5) "
        "  private owned _s: Res := r; "
        "  private _t: int := tag; "
        "  public constructor(owned x: Res, flag: bool) "
        "    this(move x); "
        "  end constructor; "
        "  public method Id(): int "
        "    return _t + _s.Id(); "
        "  end Id; "
        "end Base; " +
        loop200("using var b := new Base(new Res(i), true); ");
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("acc=20900 made=200 live=0") !=
                      std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// R5 — REJECT. A NAMED argument filling the `owned` parameter while the
// defaulted one is omitted. §22.5: "Named arguments may appear in any order
// after positional arguments"; the classification must key off the BOUND
// parameter, which is what `boundParamIndex` resolves — and it could not,
// because the view it resolves against did not exist.
// FAIL-WITHOUT: accepted with no diagnostic (measured on 89eb914).
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5 (named-argument binding) / §22.3, issue #3769
BEZEL_TEST(defaulted_ctor_arity_owned_sink_3769,
           new_site_named_argument_binds_owned_formal_across_omitted_default_rejected) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kResPlain + kBasePrimaryOwnedThenDefault +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(6); "
        "    using var b := new Base(r := x); "
        "    return b.Id(); "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(hasMissingOwnedMarker(r, "x"));
}

// ═══════════════════════════════════════════════════════════════════════════
// R6 — ACCEPT, ASan/LSan, 200 iterations. The correct spelling of R5. §22.5:
// "A transfer-marked argument carries no modifier ... so `M(item := move x)` is
// an ordinary named argument whose expression happens to be a `MoveExpr`, and
// is legal (v0.260)".
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5 / §28.8.1 (the `move` expression), issue #3769
BEZEL_TEST(defaulted_ctor_arity_owned_sink_3769,
           new_site_named_move_marker_across_omitted_default_asan_x200) {
    const std::string src =
        std::string("module M3769f; ") + kRes + kBasePrimaryOwnedThenDefault +
        loop200("var x := new Res(i); "
                "using var b := new Base(r := move x); ");
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("acc=20900 made=200 live=0") !=
                      std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// R7 — REJECT. An EXPLICIT constructor reached by filling its §22.5 default,
// on a class that ALSO has a §22.10 primary constructor of the call's exact
// arity. Codegen's `ctorForArgs` tries explicit-fill BEFORE the primary, so
// ownership must too or it would classify against the wrong parameter list.
// Measured on this commit: the program prints `4/6` — `n` defaulted to 4 and
// the chain stored it into the primary's `a` — which is the explicit
// constructor, not the primary.
// FAIL-WITHOUT: accepted with no diagnostic (measured on 89eb914).
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5 / §22.6 (resolution pipeline) / §22.10, issue #3769
BEZEL_TEST(defaulted_ctor_arity_owned_sink_3769,
           new_site_explicit_ctor_defaulted_fill_beats_primary_exact_arity_rejected) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kResPlain +
        "class B(a: int) "
        "  private owned _f: Res; "
        "  public constructor(owned r: Res, n: int := 4) "
        "    this(n); "
        "    _f := move r; "
        "  end constructor; "
        "  public method Id(): int "
        "    return a + _f.Id(); "
        "  end Id; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(6); "
        "    using var b := new B(x); "
        "    return b.Id(); "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(hasMissingOwnedMarker(r, "x"));
}

// ═══════════════════════════════════════════════════════════════════════════
// R8 — ACCEPT, ASan/LSan, 200 iterations. The correct spelling of R7, and the
// proof that the constructor ownership picked is the one codegen calls: `acc`
// counts `a + _f.Id()` where `a` is the DEFAULT `4` routed through the
// explicit constructor's own `this(n)` chain.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5 / §22.10 / §28.8, issue #3769
BEZEL_TEST(defaulted_ctor_arity_owned_sink_3769,
           new_site_explicit_ctor_defaulted_fill_transfers_asan_x200) {
    const std::string src =
        std::string("module M3769h; ") + kRes +
        "class B(a: int) "
        "  private owned _f: Res; "
        "  public constructor(owned r: Res, n: int := 4) "
        "    this(n); "
        "    _f := r; "
        "  end constructor; "
        "  public method Id(): int "
        "    return a + _f.Id(); "
        "  end Id; "
        "end B; " +
        loop200("var x := new Res(i); "
                "using var b := new B(move x); ");
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // acc = sum(4 + i, i in 0..199) = 800 + 19900.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("acc=20700 made=200 live=0") !=
                      std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// R9 — ANTI-OVER-REJECT (passes on BOTH sides, by design). §22.6's
// "Defaulted-arity tie-break": "a candidate applicable **without filling any
// default** is preferred over a candidate that fills one or more". The
// EXACT-arity constructor here takes a plain (borrowing) `Res`, so the argument
// is NOT consumed and no marker is required; only if the fix had put the
// defaults pass ahead of the exact pass would this become a false reject.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.6 (Defaulted-arity tie-break) / §22.5, issue #3769
BEZEL_TEST(defaulted_ctor_arity_owned_sink_3769,
           exact_arity_candidate_still_preferred_over_defaulted_fill) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kResPlain +
        "class B "
        "  private owned _f: Res; "
        "  private _n: int; "
        "  public constructor(r: Res) "
        "    _f := new Res(); "
        "    _n := r.Id(); "
        "  end constructor; "
        "  public constructor(owned r2: Res, n: int := 4) "
        "    _f := r2; "
        "    _n := n; "
        "  end constructor; "
        "  public method Id(): int => _n; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    using var x := new Res(); "
        "    using var b := new B(x); "
        "    return b.Id(); "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(hasMissingOwnedMarker(r, "x"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ═══════════════════════════════════════════════════════════════════════════
// R10 — ANTI-OVER-REJECT (passes on BOTH sides, by design). A §22.10 primary
// whose parameters are all PLAIN, called with the defaulted tail omitted. The
// fix newly RESOLVES a formal list here where none resolved before, so every
// consumer of that list becomes newly reachable for this shape; nothing about
// it may become an owned sink, a `scoped` position, or a §28.14 call-bounded
// position. The `2/1` output also pins that the §22.5 default is still
// materialized.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5 (Evaluation timing — the default materializes per call) / §22.10,
// issue #3769
BEZEL_TEST(defaulted_ctor_arity_owned_sink_3769,
           plain_positional_omitted_default_is_not_an_owned_sink_accepted) {
    auto r = runAot(
        "module M3769j; "
        "class P(a: int, b: int := 1) "
        "  public method Show(): string => $\"{a}/{b}\"; "
        "end P; "
        "class C "
        "  public static method Main(): int "
        "    var p := new P(2); "
        "    Console.WriteLine(p.Show()); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("2/1") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// R11 — REJECT + ACCEPT, and the row that pins `CtorSelect::ChainSite`.
//
// ⚠ REVERSED BY fix #3777, IN THE SAME BATCH. When #3769 landed, a `this(...)`
// chain to an explicit SIBLING across an omitted default was not lowered by
// codegen at all — `emitChainCall` resolved its target with `ctorByArity`
// (exact arity, no defaults) and the `!target` fall-through reached the §22.10
// primary arm, which this class does not have; the plain twin printed `0/0`.
// Ownership therefore declined to classify `x` as transferred: demanding
// `this(owned x)` would have suppressed the §28.3 auto-drop and leaked the
// object the never-called constructor was supposed to adopt (the reviewer
// measured 800 B / 50 objects for the flip alone), and this row asserted the
// ACCEPT.
//
// fix #3777 makes `emitChainCall` select with `ctorForArgs` and materialise the
// omitted §22.5 tail, so `B.constructor(owned a: Res, b: int := 3)` really runs
// and really adopts the argument into `_f`. The carve-out inverts into a DOUBLE
// FREE — measured on the codegen half alone, `AddressSanitizer:
// heap-use-after-free`, freed in `bz$M.Holder$ctor.1` — so the two halves land
// together and this row flips with them: unmarked is a §22.3 REJECT, and
// `this(owned x)` is the spelling that disposes exactly once.
// ═══════════════════════════════════════════════════════════════════════════
// per §12.8 / §22.5 / §22.3 / §28.8, issues #3769 and #3777
BEZEL_TEST(defaulted_ctor_arity_owned_sink_3769,
           chain_to_explicit_sibling_across_omitted_default_is_an_owned_sink) {
    const char* bodyUnmarked =
        "class B "
        "  private owned _f: Res; "
        "  private _n: int; "
        "  public constructor(owned a: Res, b: int := 3) "
        "    _f := a; "
        "    _n := b; "
        "  end constructor; "
        "  public constructor(owned x: Res, y: bool) "
        "    this(x); "
        "  end constructor; "
        "end B; ";
    auto d = parseAndCheckOwnership(std::string("module M; ") + kResPlain +
                                    bodyUnmarked);
    // The sibling constructor is really called now, so the position IS an owned
    // sink and §22.3 requires the call site to announce the transfer.
    BEZEL_EXPECT_TRUE(hasMissingOwnedMarker(d, "x"));

    // The MARKED spelling is accepted, and disposes exactly once: `x` is
    // moved-from, so §28.3's auto-drop does not fire, and `B._f` owns the
    // object for the rest of the `using var`'s scope.
    const char* bodyMarked =
        "class B "
        "  private owned _f: Res; "
        "  private _n: int; "
        "  public constructor(owned a: Res, b: int := 3) "
        "    _f := a; "
        "    _n := b; "
        "  end constructor; "
        "  public constructor(owned x: Res, y: bool) "
        "    this(move x); "
        "  end constructor; "
        "  public method N(): int "
        "    return _n; "
        "  end N; "
        "end B; ";
    auto dm = parseAndCheckOwnership(std::string("module M; ") + kResPlain +
                                     bodyMarked);
    BEZEL_EXPECT_FALSE(hasMissingOwnedMarker(dm, "x"));
    BEZEL_EXPECT_FALSE(hasSpuriousOwnedMarkerReject(dm));
    BEZEL_EXPECT_FALSE(hasOwnedParamAutoDropWarning(dm, "x"));

    const std::string src =
        std::string("module M3769k; ") + kRes + bodyMarked +
        "class C "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    var acc: int := 0; "
        "    while i < 50 do "
        "      using var b := new B(new Res(i), true); "
        "      acc := acc + b.N(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"made={Res._made} live={Res._live} acc={acc}\"); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // acc = 50 * 3 proves the §22.5 default really reached `_n` — the value
    // half #3777 adds on top of the disposal counters #3769 could assert.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("made=50 live=0 acc=150") !=
                      std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// R12 — REJECT. The `base(...)` twin of R3: a derived constructor chaining into
// a BASE class's §22.10 primary across an omitted default. Same resolver, same
// hole; §12.8 puts `base(...)` in the same leading statement position.
// FAIL-WITHOUT: accepted with only the §28.3 auto-drop warning, and
// `AddressSanitizer: heap-use-after-free` at run time (measured on 89eb914).
// ═══════════════════════════════════════════════════════════════════════════
// per §12.8 / §22.10 / §22.5 / §22.3, issue #3769
BEZEL_TEST(defaulted_ctor_arity_owned_sink_3769,
           base_chain_unmarked_owned_param_across_omitted_default_rejected) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kResPlain + kBasePrimaryOwnedThenDefault +
        "class D : Base "
        "  public constructor(owned x: Res) "
        "    base(x); "
        "  end constructor; "
        "end D; ");
    BEZEL_EXPECT_TRUE(hasMissingOwnedMarker(r, "x"));
    BEZEL_EXPECT_FALSE(hasOwnedParamAutoDropWarning(r, "x"));
}

// ═══════════════════════════════════════════════════════════════════════════
// R13 — ACCEPT, ASan/LSan, 200 iterations. The correct spelling of R12.
// FAIL-WITHOUT: rejected ("this parameter position does not take ownership").
//
// ⚠ The oracle here is `made`/`live` ONLY — exactly-once construction and
// disposal, which is the §28.8 question this fix answers. The base primary's
// §22.5 DEFAULT is NOT asserted, because codegen does not fill it at a
// `base(...)` chain: measured on this commit, `_t` reads back 0 rather than 5.
// That gap is pre-existing and independent of ownership (its plain twin,
// `class P(n: int, s: string := "d")` reached by `base(v)`, SEGVs identically
// on 89eb914 and on this commit); it is filed separately. Asserting the value
// here would pin a codegen defect into an ownership row.
// ═══════════════════════════════════════════════════════════════════════════
// per §12.8 / §22.10 / §22.3 / §28.8, issue #3769
BEZEL_TEST(defaulted_ctor_arity_owned_sink_3769,
           base_chain_owned_marker_across_omitted_default_asan_x200) {
    const std::string src =
        std::string("module M3769m; ") + kRes +
        "class Base(owned r: Res, tag: int := 5) "
        "  private owned _s: Res := r; "
        "  private _t: int := tag; "
        "  public method Id(): int "
        "    return _s.Id(); "
        "  end Id; "
        "end Base; "
        "class D : Base "
        "  public constructor(owned x: Res) "
        "    base(move x); "
        "  end constructor; "
        "end D; " +
        loop200("using var b := new D(new Res(i)); ");
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("acc=19900 made=200 live=0") !=
                      std::string::npos);
}
