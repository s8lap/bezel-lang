// §12.8 / §22.10 (issue #4066, v0.294): `gateChainPositionFieldRead`'s class-
// only scope. The two class-only axes that survived fix #4014 (which gated
// rule 1's this-observation gate on `recordCtorMode_` for records) and
// fix #4060 (which made `runRecordConstructor` actually walk its chain-call
// arguments through `visitCallArg`) are:
//
//   1. The activation flag at the top of the function:
//      `if (!ctorMode_ || !inChainingPosition_) return;`
//   2. The declaring-type check on each arm:
//      `if (!fs->parent || fs->parent->kind != ast::TypeDeclKind::Class)`
//      (and the PropertySymbol analogue).
//
// §12.8 / §22.10's rule — "a `this(...)`/`base(...)` argument runs before
// any field of the declaring class is assigned, so [a field or auto-
// property read] has no value here" — has nothing class-specific in it: the
// underlying fact is "the chaining position is outside the body", and that
// is independent of whether the declaring type is a class or a record. The
// record's own §22.8 sequencing reads identically (no body, fields assigned
// by the chained-to primary constructor AFTER the chain arguments evaluate).
//
// Before this fix, both axes are class-only, so a record's
// `this(this.Field)` (or `this(this.Prop)`, or the same nested under
// `this(this.F + 1)`) compiled cleanly and SIGSEGV'd at runtime on the
// same path. The issue's exact repro:
//
//   public record R
//     public F: int;
//     public constructor(v: int) F := v; end constructor;
//     public constructor() this(this.F); end constructor;
//   end R;
//
// is the REJECT row below (Row 1).
//
// THE FIX. `gateChainPositionFieldRead` is now gated on
// `(ctorMode_ || recordCtorMode_)` and admits records in the
// `fs->parent->kind`/`ps->parent->kind` checks on both arms — mirroring the
// parallel-record-arm pattern already applied in fix #4014 for the
// gateObservation/visitCtorCall/visitCtorMemberAccess trio. The diagnostic
// text "any field of the declaring class is assigned" is generalised to
// "any field of the declaring type is assigned" because records also
// reach it now — the spec text (§12.8 / §22.10) is type-agnostic.
//
// FAIL-WITHOUT/PASS-WITH: measured precisely. Reverting
// `DefiniteAssignment.cpp` to its pre-fix state fails the 5 REJECT-arm
// tests below (`this_this_field_read_rejected`, `this_this_qualified_field_
// read_rejected`, `this_this_property_read_rejected`,
// `this_this_nested_field_read_rejected`,
// `primary_param_field_read_still_rejected`) — the first four compile
// clean pre-fix and crash at runtime, the fifth is unaffected by this fix
// but correctly rejected on both sides (unaffected parity pin).
// `class_control_this_this_field_read_rejected` likewise passes on both
// sides (an unaffected parity pin: `runConstructor` was never touched, and
// the diagnostic message text change "class" -> "type" is the only
// observable difference on the class side, which neither this file's
// existing `hasError` checks nor the 3829 file's `hasError` checks assert
// on). The 4 ACCEPT-arm/control rows (`own_parameter_control_accepted`,
// `static_field_control_accepted`, `nested_accept_control_accepted`,
// `multi_level_chain_control_accepted`) pass on BOTH sides by design — the
// fix only widens a previously-class-only rejection to also reject on
// records, so any accept row that was already accepted continues to be
// accepted, and the value of pinning them is to guard against a future
// over-rejection (LANE_RULES §15 accept-side sweep).

#include <cstdio>
#include <regex>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// True iff SOME diagnostic contains `sub`. Never assert on the coarse
// `hasErrors()` alone (LANE_RULES §3) — pin the specific message text.
bool hasError(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// Real-location check (LANE_RULES §18): `runAot` does NOT hand back one
// structured `Diagnostic` per compiler message on a REJECT — on
// `CompileOutcome::Rejected` it packs the whole captured driver stderr into
// a SINGLE synthetic `Diagnostic` whose `.location` is default-constructed
// `SourceLocation{}` — so `d.location` is {0,0} for EVERY AOT reject test
// regardless of what the compiler itself reported. The real per-diagnostic
// location survives as TEXT: `Driver.cpp` prints each diagnostic as
// `<path>:<line>:<col>: <message>`, and that text is exactly what lands in
// the packed message. So the real check parses the "<path>:<line>:<col>: "
// prefix off the specific line that carries `sub` and asserts both numbers
// are non-zero. Mirrors chain_position_field_read_3829_test.cpp's identical
// helper/rationale.
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

constexpr const char* kSection12_8_22_10 = "§12.8, §22.10";

}  // namespace

// ---------------------------------------------------------------------------
// Row 1 — REJECT (record): the issue's own repro. A bare field read of
// `this.Field` in the chain-call position on a record. Before #4066 this
// compiled cleanly and SIGSEGV'd at runtime on the same axis as
// chain_position_field_read_3829_test.cpp's Row 1 (which covers the class
// side). After #4066 it is rejected at compile time with the same diagnostic
// shape §3829 emits for a class.
// ---------------------------------------------------------------------------
BEZEL_TEST(gate_chain_position_field_read_record_4066, this_this_field_read_rejected) {
    auto r = runAot(
        "module M; "
        "public record R4066 "
        "  public F: int; "
        "  public constructor(v: int) F := v; end constructor; "
        "  public constructor() this(this.F); end constructor; "  // reads F in the chaining position
        "end R4066; "
        "class C "
        "  public static method Main(): int "
        "    var r := new R4066(); "
        "    return r.F; "
        "  end Main; "
        "end C;");
    if (!hasError(r, "cannot read field `F`") ||
        !hasError(r, kSection12_8_22_10))
        dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, "cannot read field `F`"));
    BEZEL_EXPECT_TRUE(hasError(r, "chaining position"));
    BEZEL_EXPECT_TRUE(hasError(r, kSection12_8_22_10));
    BEZEL_EXPECT_TRUE(hasErrorWithRealLocation(r, "cannot read field `F`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// Row 2 — REJECT (record): the `this`-qualified spelling of Row 1's read.
// Both spellings resolve to the same FieldSymbol, so both must be rejected
// identically — pins that the `this`-qualifier never bypasses the gate.
// ---------------------------------------------------------------------------
BEZEL_TEST(gate_chain_position_field_read_record_4066, this_this_qualified_field_read_rejected) {
    auto r = runAot(
        "module M; "
        "public record R4066q "
        "  public F: int; "
        "  public constructor(v: int) F := v; end constructor; "
        "  public constructor() this(this.F); end constructor; "
        "end R4066q; "
        "class C "
        "  public static method Main(): int "
        "    var r := new R4066q(); "
        "    return r.F; "
        "  end Main; "
        "end C;");
    if (!hasError(r, "cannot read field `F`")) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, "cannot read field `F`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// Row 3 — REJECT (record): an auto-property's backing storage read in the
// chain-call position. Mirrors chain_position_field_read_3829_test.cpp's
// Row 3 on the class side. The PropertySymbol arm's parallel record check
// must fire here.
// ---------------------------------------------------------------------------
BEZEL_TEST(gate_chain_position_field_read_record_4066, this_this_property_read_rejected) {
    auto r = runAot(
        "module M; "
        "public record R4066p "
        "  public property P: int; "
        "  public constructor(v: int) P := v; end constructor; "
        "  public constructor() this(this.P); end constructor; "
        "end R4066p; "
        "class C "
        "  public static method Main(): int "
        "    var r := new R4066p(); "
        "    return r.P; "
        "  end Main; "
        "end C;");
    if (!hasError(r, "cannot read auto-property `P`") ||
        !hasError(r, kSection12_8_22_10))
        dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, "cannot read auto-property `P`"));
    BEZEL_EXPECT_TRUE(hasError(r, kSection12_8_22_10));
    BEZEL_EXPECT_TRUE(hasErrorWithRealLocation(r, "cannot read auto-property `P`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// Row 4 — REJECT (record): a field read NESTED inside a chain argument.
// The walk must not stop at the argument's top-level expression node —
// mirrors chain_position_field_read_3829_test.cpp's Row 4 on the class
// side. Pre-#4066: walks into the argument list (fix #4060) but never
// reaches the gate (class-only #4066); post-#4066: gate fires.
// ---------------------------------------------------------------------------
BEZEL_TEST(gate_chain_position_field_read_record_4066, this_this_nested_field_read_rejected) {
    auto r = runAot(
        "module M; "
        "public record R4066n "
        "  public F: int; "
        "  public constructor(v: int) F := v; end constructor; "
        "  public constructor() this(this.F + 1); end constructor; "
        "end R4066n; "
        "class C "
        "  public static method Main(): int "
        "    var r := new R4066n(); "
        "    return r.F; "
        "  end Main; "
        "end C;");
    if (!hasError(r, "cannot read field `F`")) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, "cannot read field `F`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// Row 5 — REJECT (class): unaffected parity pin. `runConstructor` is never
// touched by this fix, the class side was already correctly rejected, and
// the only observable change to the class-side diagnostic is "declaring
// class" -> "declaring type" (more accurate for the record arm, but correct
// for the class arm too — §12.8/§22.10 does not single out classes). This
// row pins that the class path still rejects identically. The text check
// here deliberately does NOT include "declaring class" — the assertion is on
// `cannot read field` and `§12.8, §22.10`, both unchanged on the class
// side.
// ---------------------------------------------------------------------------
BEZEL_TEST(gate_chain_position_field_read_record_4066, class_control_this_this_field_read_rejected) {
    auto r = runAot(
        "module M; "
        "class B4066 "
        "  private _f: int := 5; "
        "  public constructor(v: int, w: int) _f := w; end constructor; "
        "  public constructor(v: int) this(v, _f); end constructor; "
        "end B4066; "
        "class C "
        "  public static method Main(): int "
        "    var b := new B4066(5); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (!hasError(r, "cannot read field `_f`")) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, "cannot read field `_f`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// Row 6 — REJECT (class): unaffected parity pin for the class-side primary-
// constructor-parameter arm — `runConstructor` is never touched, the class
// path was always correctly rejected, and changing the gate's activation
// to `ctorMode_ || recordCtorMode_` does not affect it.
// ---------------------------------------------------------------------------
BEZEL_TEST(gate_chain_position_field_read_record_4066, primary_param_field_read_still_rejected) {
    auto r = runAot(
        "module M; "
        "class B4066p(n: int, s: string) "
        "  public constructor(v: int) this(v, s); end constructor; "  // reads primary-ctor param `s`
        "end B4066p; "
        "class C "
        "  public static method Main(): int "
        "    var b := new B4066p(5, \"x\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (!hasError(r, "cannot read primary-constructor parameter `s`"))
        dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, "cannot read primary-constructor parameter `s`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// Row 7 — CONTROL, ACCEPT (record): the chaining constructor's own parameter
// is admitted — §12.8's positive list ("the constructor's own parameters").
// Pin so a future over-rejection does not silently start rejecting
// parameter passes. Mirrors chain_position_field_read_3829_test.cpp's Row 6.
// ---------------------------------------------------------------------------
BEZEL_TEST(gate_chain_position_field_read_record_4066, own_parameter_control_accepted) {
    auto r = runAot(
        "module M; "
        "public record R4066o "
        "  public F: int; "
        "  public constructor(v: int) F := v; end constructor; "
        "  public constructor(a: int, b: int) this(a + b); end constructor; "
        "end R4066o; "
        "class C "
        "  public static method Main(): int "
        "    var r := new R4066o(1, 2); "
        "    return r.F; "
        "  end Main; "
        "end C;");
    if (r.diagnostics.hasErrors()) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ---------------------------------------------------------------------------
// Row 8 — CONTROL, ACCEPT (record): a literal chain argument — §12.8's
// positive list ("the constructor's own parameters, constants, and statics")
// admits constants/statics, and a literal is the simplest non-symbol read
// the gate cannot affect. This is overwhelmingly the most common real-world
// record-chain shape (LANE_RULES §15 accept-side sweep).
// ---------------------------------------------------------------------------
BEZEL_TEST(gate_chain_position_field_read_record_4066, literal_chain_control_accepted) {
    auto r = runAot(
        "module M; "
        "public record R4066s "
        "  public F: int; "
        "  public constructor(v: int) F := v; end constructor; "
        "  public constructor() this(42); end constructor; "
        "end R4066s; "
        "class C "
        "  public static method Main(): int "
        "    var r := new R4066s(); "
        "    return r.F; "
        "  end Main; "
        "end C;");
    if (r.diagnostics.hasErrors()) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ---------------------------------------------------------------------------
// Row 9 — CONTROL, ACCEPT (record): a chain argument with NO field/auto-
// property reference at all (only an arithmetic on the chaining ctor's own
// parameter) is admitted — the gate never reaches it because no FieldSymbol
// or auto-property-storage PropertySymbol is read. Mirrors
// chain_position_field_read_3829_test.cpp's Row 9 on the class side.
// ---------------------------------------------------------------------------
BEZEL_TEST(gate_chain_position_field_read_record_4066, nested_accept_control_accepted) {
    auto r = runAot(
        "module M; "
        "public record R4066c "
        "  public F: int; "
        "  public constructor(v: int) F := v; end constructor; "
        "  public constructor(a: int, b: int) this((a + b) * 2); end constructor; "
        "end R4066c; "
        "class C "
        "  public static method Main(): int "
        "    var r := new R4066c(1, 2); "
        "    return r.F; "
        "  end Main; "
        "end C;");
    if (r.diagnostics.hasErrors()) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);
}

// ---------------------------------------------------------------------------
// Row 10 — CONTROL, ACCEPT (record): a chain to a constructor that itself
// chains (multi-level delegation: the no-arg ctor chains to the two-arg
// ctor, which itself chains to the one-arg ctor). Exercises
// `inChainingPosition_`/`visitCallArg` composing correctly across more than
// one constructor walked independently by the DA pass, plus the gate's
// own `(ctorMode_ || recordCtorMode_)` activation across all three walks.
// ---------------------------------------------------------------------------
BEZEL_TEST(gate_chain_position_field_read_record_4066, multi_level_chain_control_accepted) {
    auto r = runAot(
        "module M; "
        "public record R4066m "
        "  public F: int; "
        "  public constructor(v: int) F := v; end constructor; "
        "  public constructor(a: int, b: int) this(a + b); end constructor; "
        "  public constructor() this(1, 2); end constructor; "
        "end R4066m; "
        "class C "
        "  public static method Main(): int "
        "    var r := new R4066m(); "
        "    return r.F; "
        "  end Main; "
        "end C;");
    if (r.diagnostics.hasErrors()) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}