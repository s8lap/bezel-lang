// §22.7 / §22.3 / §29.11 (issue #4060, v0.294): a record's own `this(...)`
// chain-call arguments got NO definite-assignment walk at all — not merely a
// missing this-observation check (that half is issue #4014's territory), but
// the ENTIRE walk: `DefiniteAssignment.cpp::runRecordConstructor` computed
// `startIdx`/`isThisChain` from `getChainingCall` but discarded the returned
// `ast::CallExpr*` and never visited a single one of its arguments — no
// `visitCallArg`, no `inChainingPosition_`, nothing. `runConstructor` (the
// class analogue, a few dozen lines above in the same file) has always
// visited its own chain-call arguments through `visitCallArg` under
// `inChainingPosition_ = true`.
//
// WHAT THE DEFECT WAS. Because the record path's argument list was never
// walked by ANYTHING, every ordinary check `visitCallArg` performs for every
// OTHER call's arguments was silently skipped specifically for a record's
// chain-call arguments: local read-before-assignment (§21.1/§7.9), §22.3
// inline `out var` tracking (including the path-sensitive short-circuit
// narrowing spelled out in §22.4 — "assigned only on paths where the
// introducing call actually evaluated"), and §29.11 lazy-query consumption.
// The issue's own measured repro is a SIGSEGV via `this(this.Peek())`
// reading an unassigned reference field through a method call — but closing
// THAT specific hazard also requires threading `recordCtorMode_` into the
// this-observation gate (`gateObservation`/`visitCtorCall`/
// `visitCtorMemberAccess`, issue #4014, and NOT included by this fix — see
// the composition note at the bottom of this file) plus, independently,
// widening `gateChainPositionFieldRead` to admit records at all (still
// class-scoped as of this fix — filed separately, see below). This fix's
// OWN, independently-measurable effect is the ordinary local-DA/§22.3
// tracking demonstrated below, proven FAIL-WITHOUT/PASS-WITH against
// `DefiniteAssignment.cpp` reverted to its pre-fix (origin/main a6e270d)
// state.
//
// THE FIX. `runRecordConstructor` now captures `getChainingCall`'s returned
// `ast::CallExpr*`, sets `inChainingPosition_ = true`, and calls
// `visitCallArg` on every argument before restoring
// `inChainingPosition_ = false` — mirroring `runConstructor`'s identical
// block verbatim (down to the seeding of `assignedFields_`/
// `readonlyMaybeAssigned_` immediately after, whose ordering is unchanged).
//
// Rows 1-2 below pin the reachable local-DA improvement (a chain call must
// be a constructor's first statement per §12.8, so no PRE-DECLARED local can
// ever reach the chain position — the only way an inline declaration can be
// misused there is through an `out var`/short-circuit interaction WITHIN the
// chain call's own argument list, which is exactly what these rows exploit).
// Rows 3-5 sweep the accept side (LANE_RULES §15): a chain call must keep
// compiling correctly for the ordinary shapes that do nothing wrong.

#include <cstdio>
#include <regex>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasError(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// Real-location check (LANE_RULES §18): the pre-fix behaviour for the
// this-observation half of this hazard is a SIGSEGV, whose own backstop
// diagnostic can carry a real location too — so every reject row here pairs
// this helper with an explicit message-text assertion (`hasError` above),
// never relying on the location check alone. Mirrors
// chain_position_field_read_3829_test.cpp's identical helper/rationale.
bool hasErrorWithRealLocation(const JitResult& r, const char* sub) {
    static const std::regex kLoc(R"(:(\d+):(\d+): )");
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::size_t pos = d.message.find(sub);
        if (pos == std::string::npos) continue;
        std::size_t lineStart = d.message.rfind('\n', pos);
        lineStart = (lineStart == std::string::npos) ? 0 : lineStart + 1;
        std::size_t lineEnd = d.message.find('\n', pos);
        std::string line = d.message.substr(
            lineStart, (lineEnd == std::string::npos ? d.message.size()
                                                      : lineEnd) -
                           lineStart);
        std::smatch m;
        if (!std::regex_search(line, m, kLoc)) return false;
        int lineNo = std::stoi(m[1].str());
        int colNo = std::stoi(m[2].str());
        return lineNo != 0 && colNo != 0;
    }
    return false;
}

void dumpDiagnostics(const JitResult& r) {
    std::fprintf(stderr, "  diagnostics (count=%zu):\n",
                 r.diagnostics.diagnostics().size());
    for (const auto& d : r.diagnostics.diagnostics())
        std::fprintf(stderr, "    [%u:%u] %s\n", d.location.line,
                     d.location.column, d.message.c_str());
}

}  // namespace

// ---------------------------------------------------------------------------
// Row 1 — REJECT (record): an inline `out var` declared inside a chain
// call's own argument list, read again on a path where the introducing call
// may not have run — `(cond and TryGet(out var w)) or Check(w)` reads `w`
// on the `or`'s right operand, which per §22.4 evaluates exactly when the
// left operand (and therefore `TryGet`) was false, i.e. exactly the path on
// which `w` (a non-nullable reference type) is NOT definitely assigned.
// Before this fix `runRecordConstructor` never called `visitCallArg` on the
// chain call's arguments at all, so this compiled clean (silently, no
// DA check of any kind reached the argument list — measured on origin/main
// a6e270d: compiles clean, exit 0, runs without crashing for THIS input
// only because `cond` happens to be `true` at the one call site in the
// fixture; the compile-time acceptance itself is the defect, not this
// particular run).
// ---------------------------------------------------------------------------
BEZEL_TEST(record_ctor_chain_arg_da_walk_4060, out_var_unguarded_read_rejected) {
    auto r = runAot(
        "module M; "
        "class Widget4060 "
        "  public Val: int; "
        "  public constructor(v: int) Val := v; end constructor; "
        "end Widget4060; "
        "public record RecordChainOutVar4060 "
        "  public Flag: bool; "
        "  public constructor(f: bool, tag: int) Flag := f; end constructor; "
        "  public constructor(cond: bool) "
        "    this((cond and TryGet(out var w)) or Check(w), 0); "
        "  end constructor; "
        "  private static method TryGet(out r: Widget4060): bool "
        "    r := new Widget4060(5); "
        "    return true; "
        "  end TryGet; "
        "  private static method Check(w: Widget4060): bool "
        "    return w.Val > 0; "
        "  end Check; "
        "end RecordChainOutVar4060; "
        "class C "
        "  public static method Main(): int "
        "    var c := new RecordChainOutVar4060(true); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (!hasError(r, "read of unassigned local `w`")) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, "read of unassigned local `w`"));
    BEZEL_EXPECT_TRUE(hasErrorWithRealLocation(r, "read of unassigned local `w`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// Row 2 — CONTROL (class): the character-identical shape on the class side,
// unaffected by this fix (`runConstructor` already walked its chain args),
// already correctly rejects it on origin/main a6e270d. Pins that the
// record's new diagnostic (row 1) is genuine parity with existing,
// established behaviour rather than a novel rejection rule invented here.
// ---------------------------------------------------------------------------
BEZEL_TEST(record_ctor_chain_arg_da_walk_4060, class_control_out_var_unguarded_read_rejected) {
    auto r = runAot(
        "module M; "
        "class Widget4060b "
        "  public Val: int; "
        "  public constructor(v: int) Val := v; end constructor; "
        "end Widget4060b; "
        "public class ClassChainOutVar4060 "
        "  public Flag: bool; "
        "  public constructor(f: bool, tag: int) Flag := f; end constructor; "
        "  public constructor(cond: bool) "
        "    this((cond and TryGet(out var w)) or Check(w), 0); "
        "  end constructor; "
        "  private static method TryGet(out r: Widget4060b): bool "
        "    r := new Widget4060b(5); "
        "    return true; "
        "  end TryGet; "
        "  private static method Check(w: Widget4060b): bool "
        "    return w.Val > 0; "
        "  end Check; "
        "end ClassChainOutVar4060; "
        "class C "
        "  public static method Main(): int "
        "    var c := new ClassChainOutVar4060(true); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (!hasError(r, "read of unassigned local `w`")) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, "read of unassigned local `w`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// Row 3 — CONTROL, ACCEPT (record): the ordinary case — a chain call passing
// only the chaining constructor's own parameters — must keep compiling and
// running correctly (LANE_RULES §15 accept-side sweep). This is overwhelmingly
// the common real-world shape and must not regress.
// ---------------------------------------------------------------------------
BEZEL_TEST(record_ctor_chain_arg_da_walk_4060, own_parameter_control_accepted) {
    auto r = runAot(
        "module M; "
        "public record RecordChainOwnParam4060 "
        "  public N: int; "
        "  public constructor(n: int) N := n; end constructor; "
        "  public constructor(a: int, b: int) this(a + b); end constructor; "
        "end RecordChainOwnParam4060; "
        "class C "
        "  public static method Main(): int "
        "    var c := new RecordChainOwnParam4060(1, 2); "
        "    return c.N; "
        "  end Main; "
        "end C;");
    if (r.diagnostics.hasErrors()) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ---------------------------------------------------------------------------
// Row 4 — CONTROL, ACCEPT (record): a chain to a constructor that itself
// chains (multi-level delegation: the no-arg ctor chains to the two-arg
// ctor, which itself chains to the one-arg ctor). Exercises
// `inChainingPosition_`/`visitCallArg` composing correctly across more than
// one constructor walked independently by the DA pass.
// ---------------------------------------------------------------------------
BEZEL_TEST(record_ctor_chain_arg_da_walk_4060, multi_level_chain_control_accepted) {
    auto r = runAot(
        "module M; "
        "public record RecordChainMultiLevel4060 "
        "  public N: int; "
        "  public constructor(n: int) N := n; end constructor; "
        "  public constructor(a: int, b: int) this(a + b); end constructor; "
        "  public constructor() this(1, 2); end constructor; "
        "end RecordChainMultiLevel4060; "
        "class C "
        "  public static method Main(): int "
        "    var c := new RecordChainMultiLevel4060(); "
        "    return c.N; "
        "  end Main; "
        "end C;");
    if (r.diagnostics.hasErrors()) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ---------------------------------------------------------------------------
// Row 5 — CONTROL, ACCEPT (record): a lambda that captures `this` inside a
// chain-call argument. §22.7 rule 3 states a lambda capture is not itself an
// observation of `this` (the observation, if any, happens later — whenever
// the lambda is invoked, which the DA pass cannot see through in general).
// Now that `visitCallArg` actually walks this argument, it must still admit
// this shape rather than misfiring `inChainingPosition_` against the
// lambda's OWN (deferred) body.
// ---------------------------------------------------------------------------
BEZEL_TEST(record_ctor_chain_arg_da_walk_4060, lambda_capture_control_accepted) {
    auto r = runAot(
        "module M; "
        "public record RecordChainLambdaCapture4060 "
        "  public N: int; "
        "  public constructor(n: int) N := n; end constructor; "
        "  public constructor() "
        "    this(Compute(lambda() => 5)); "
        "  end constructor; "
        "  private static method Compute(f: () -> int): int return f(); end Compute; "
        "end RecordChainLambdaCapture4060; "
        "class C "
        "  public static method Main(): int "
        "    var c := new RecordChainLambdaCapture4060(); "
        "    return c.N; "
        "  end Main; "
        "end C;");
    if (r.diagnostics.hasErrors()) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ---------------------------------------------------------------------------
// COMPOSITION NOTE (not a test): this fix does NOT by itself close the
// issue's own `this(this.Peek())` SIGSEGV repro, nor the sibling
// `this(this.Field)` / `this(this.Prop)` shapes — measured on this fix alone
// (origin/main a6e270d + this commit, no other change): all three still
// compile clean and SIGSEGV at runtime, because `gateObservation`/
// `visitCtorCall`/`visitCtorMemberAccess` remain gated on `ctorMode_` alone
// (never `recordCtorMode_`) until issue #4014 lands, and
// `gateChainPositionFieldRead` remains scoped to
// `fs->parent->kind == ast::TypeDeclKind::Class` even after #4014 (#4014
// does not touch that function at all) — filed separately as the residual
// gap. This fix's own, independently-provable value is the ordinary
// local-DA/§22.3 tracking rows 1-2 demonstrate: the chain-call argument list
// is now WALKED at all, which is the necessary (but, for the this-observation
// hazard specifically, not sufficient on its own) precondition for #4014's
// gates to ever see the chain position's arguments.
