// §22.10 / §12.8 (issue #3829, v0.291): a `this(...)`/`base(...)` chaining
// argument may not read a field or auto-property of `this` — only the
// PRIMARY-CONSTRUCTOR-PARAMETER half of this rule was enforced (fix #3583);
// the general field/auto-property half was not.
//
// §12.8 ("Constructor chaining", v0.291) states the rule directly: "The
// chaining position is outside the body: the only reads a `base(...)`/
// `this(...)` argument may make are of the constructor's own parameters,
// constants, and statics; it may **not** read a field or auto-property of the
// object under construction — nor, in a class with a primary constructor, a
// primary-constructor parameter."
//
// §22.10 ("Primary constructors", v0.291) gives the same fact from the other
// side: "... as is reading any field or auto-property of the not-yet-
// constructed `this` from that position, for the same reason: a
// `this(...)`/`base(...)` argument runs before any field of the declaring
// class is assigned (§22.7)."
//
// Both quotes are unchanged from the v0.288 text issue #3829 cites — verified
// against docs/Bezel_Language_Spec.md at v0.291 word-for-word.
//
// WHAT THE DEFECT WAS. `DefiniteAssignment.cpp`'s §22.7 rule-1 observation
// gate (`gateObservation`) deliberately excludes field/auto-property reads —
// they are backing storage, not an "observation" of `this`, and have their
// own §7.9/§28.8.1 DA rules. Fix #3583 added a SECOND, narrower gate
// (`gateChainPositionPrimaryParamRead`) that caught only a primary-
// constructor parameter (which resolves to a compiler-generated FieldSymbol
// with `posParam` set) read in the chaining position. A PLAIN field or an
// auto-property's backing storage fell into neither gate, so:
//
//   class B
//     private _f: int := 5;
//     public constructor(v: int, w: int) _f := w; end constructor;
//     public constructor(v: int) this(v, _f); end constructor;
//     ...
//   end B;
//
// compiled cleanly (measured on r12-cycle-1 f19de07) — `this(v, _f)` reads
// `_f` before ANY field of the not-yet-constructed instance is assigned
// (field initializers are step 2 of the CHAINED-TO constructor, which runs
// strictly after step 1's chain-call arguments, per §22.8), so `_f` here is
// unwritten storage with no defined value.
//
// THE FIX. `gateChainPositionPrimaryParamRead` is renamed
// `gateChainPositionFieldRead` and widened to accept ANY FieldSymbol (not
// just `posParam`) or auto-property-storage PropertySymbol read in the
// chaining position — ONE predicate covering all three shapes the spec
// describes as the same fact, rather than a second, drifting one. A static
// field/auto-property is explicitly exempted (§12.8: "... constants, and
// statics" are admitted reads), and so is a `const` member (a `const` is a
// distinct `ast::ConstDecl`/symbol kind that this gate's FieldSymbol/
// PropertySymbol dynamic_casts never match).

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

// The matched diagnostic must carry a REAL source position, never `0:0`
// (LANE_RULES §3 / COMMON.md §3).
//
// `runAot` does NOT hand back one structured `Diagnostic` per compiler
// message on a REJECT: on `CompileOutcome::Rejected` it packs the whole
// captured driver stderr into a SINGLE synthetic `Diagnostic` whose
// `.location` is always default-constructed `SourceLocation{}`
// (aot_support.hpp: `r.diagnostics.report(Severity::Error, SourceLocation{},
// std::move(cc.diag))`) — so `d.location` is {0,0} for EVERY AOT reject test
// in this codebase, regardless of what the compiler itself reported, and
// checking it directly cannot distinguish a real location from a fabricated
// one. The real per-diagnostic location survives as TEXT: `Driver.cpp`
// prints each diagnostic as `<path>:<line>:<col>: <message>` (Driver.cpp:632,
// :644), and that text is exactly what lands in the packed message. So the
// real check parses the "<path>:<line>:<col>: " prefix off the specific
// line that carries `sub` and asserts both numbers are non-zero.
bool hasErrorWithRealLocation(const JitResult& r, const char* sub) {
    static const std::regex kLoc(R"(:(\d+):(\d+): )");
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::size_t pos = d.message.find(sub);
        if (pos == std::string::npos) continue;
        // The prefix is on the same line as `sub` — walk back to the start
        // of that line, then match the location prefix within it.
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
// Row 1 — REJECT: a bare field read in the chaining position. The issue's
// own repro.
// ---------------------------------------------------------------------------
BEZEL_TEST(chain_position_field_read_3829, bare_field_read_rejected) {
    auto r = runAot(
        "module M; "
        "class B "
        "  private _f: int := 5; "
        "  public constructor(v: int, w: int) "
        "    _f := w; "
        "  end constructor; "
        "  public constructor(v: int) "
        "    this(v, _f); "  // reads _f in the chaining position
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(_f.ToString()); "
        "  end Show; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    var b := new B(5); "
        "    b.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (!hasError(r, "cannot read field `_f`") ||
        !hasError(r, kSection12_8_22_10))
        dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, "cannot read field `_f`"));
    BEZEL_EXPECT_TRUE(hasError(r, "chaining position"));
    BEZEL_EXPECT_TRUE(hasError(r, kSection12_8_22_10));
    BEZEL_EXPECT_TRUE(hasErrorWithRealLocation(r, "cannot read field `_f`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// Row 2 — REJECT: the `this._f`-qualified spelling of the same read. Both
// spellings resolve to the same FieldSymbol, so both must be rejected.
// ---------------------------------------------------------------------------
BEZEL_TEST(chain_position_field_read_3829, this_qualified_field_read_rejected) {
    auto r = runAot(
        "module M; "
        "class B "
        "  private _f: int := 5; "
        "  public constructor(v: int, w: int) "
        "    _f := w; "
        "  end constructor; "
        "  public constructor(v: int) "
        "    this(v, this._f); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(_f.ToString()); "
        "  end Show; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    var b := new B(5); "
        "    b.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (!hasError(r, "cannot read field `_f`")) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, "cannot read field `_f`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// Row 3 — REJECT: an auto-property read (bare spelling) in the chaining
// position. An auto-property's backing storage is the same unwritten-slot
// fact as a plain field's.
// ---------------------------------------------------------------------------
BEZEL_TEST(chain_position_field_read_3829, auto_property_read_rejected) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public property W: int; "
        "  public constructor(v: int, w: int) "
        "    W := w; "
        "  end constructor; "
        "  public constructor(v: int) "
        "    this(v, W); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(W.ToString()); "
        "  end Show; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    var b := new B(5); "
        "    b.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (!hasError(r, "cannot read auto-property `W`") ||
        !hasError(r, kSection12_8_22_10))
        dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, "cannot read auto-property `W`"));
    BEZEL_EXPECT_TRUE(hasError(r, kSection12_8_22_10));
    BEZEL_EXPECT_TRUE(
        hasErrorWithRealLocation(r, "cannot read auto-property `W`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// Row 4 — REJECT: the read nested inside a larger chain argument — the walk
// must not stop at the argument's top-level expression node (mirrors #3583's
// own nested-read row for the primary-parameter half).
// ---------------------------------------------------------------------------
BEZEL_TEST(chain_position_field_read_3829, nested_field_read_rejected) {
    auto r = runAot(
        "module M; "
        "class B "
        "  private _f: int := 5; "
        "  public constructor(v: int, w: int) "
        "    _f := w; "
        "  end constructor; "
        "  public constructor(v: int) "
        "    this(v, _f + 1); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(_f.ToString()); "
        "  end Show; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    var b := new B(5); "
        "    b.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (!hasError(r, "cannot read field `_f`")) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, "cannot read field `_f`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// Row 5 — REJECT: a `base(...)` chaining argument reading a field of the
// DERIVED class under construction — the spec names `base(...)` alongside
// `this(...)` throughout §12.8/§22.10, and both share `inChainingPosition_`.
// ---------------------------------------------------------------------------
BEZEL_TEST(chain_position_field_read_3829, base_chain_field_read_rejected) {
    auto r = runAot(
        "module M; "
        "class Base "
        "  public constructor(x: int) "
        "  end constructor; "
        "end Base; "
        "class Derived "
        "    : Base "
        "  private _g: int := 3; "
        "  public constructor(v: int) "
        "    base(_g); "  // reads a field of Derived's own not-yet-built `this`
        "  end constructor; "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    var d := new Derived(5); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (!hasError(r, "cannot read field `_g`")) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, "cannot read field `_g`"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// Row 6 — CONTROL, ACCEPT: the chaining constructor's OWN PARAMETER is
// admitted — §12.8's positive list ("the constructor's own parameters").
// ---------------------------------------------------------------------------
BEZEL_TEST(chain_position_field_read_3829, own_parameter_control_accepted) {
    auto r = runAot(
        "module M; "
        "class B "
        "  private _f: int; "
        "  public constructor(v: int, w: int) "
        "    _f := w; "
        "  end constructor; "
        "  public constructor(v: int) "
        "    this(v, v + 1); "  // reads only the ctor's own parameter `v`
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(_f.ToString()); "
        "  end Show; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    var b := new B(5); "
        "    b.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (r.diagnostics.hasErrors()) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("6\n"));
}

// ---------------------------------------------------------------------------
// Row 7 — CONTROL, ACCEPT: a `const` member is admitted — §12.8's positive
// list ("... constants, and statics"). A `const` is a distinct symbol kind
// from a field/auto-property (`ast::ConstDecl`, not `ast::FieldDecl`/
// `ast::PropertyDecl`), so it was never caught by either gate and needs no
// exemption inside `gateChainPositionFieldRead` — this row PINS that fact
// so a future unification does not silently start rejecting it.
// ---------------------------------------------------------------------------
BEZEL_TEST(chain_position_field_read_3829, const_control_accepted) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public const Max: int := 100; "
        "  private _f: int; "
        "  public constructor(v: int, w: int) "
        "    _f := w; "
        "  end constructor; "
        "  public constructor(v: int) "
        "    this(v, Max); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(_f.ToString()); "
        "  end Show; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    var b := new B(5); "
        "    b.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (r.diagnostics.hasErrors()) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// ---------------------------------------------------------------------------
// Row 8 — CONTROL, ACCEPT: a `static` field is admitted — §12.8's positive
// list ("... constants, and statics"). A mutable primitive static also
// carries an unrelated §22.12-rule-2 declaration-site WARNING (#1393) in
// this compiler; that is a Warning, not an Error, and must not be conflated
// with this fix's rejection.
// ---------------------------------------------------------------------------
BEZEL_TEST(chain_position_field_read_3829, static_field_control_accepted) {
    auto r = runAot(
        "module M; "
        "class B "
        "  private static _sf: int := 7; "
        "  private _f: int; "
        "  public constructor(v: int, w: int) "
        "    _f := w; "
        "  end constructor; "
        "  public constructor(v: int) "
        "    this(v, _sf); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(_f.ToString()); "
        "  end Show; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    var b := new B(5); "
        "    b.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (r.diagnostics.hasErrors()) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

// ---------------------------------------------------------------------------
// Row 9 — CONTROL, ACCEPT: a literal chain argument is admitted (no symbol
// resolution at all reaches the gate).
// ---------------------------------------------------------------------------
// Control (§12.8: "the constructor's own parameters, constants, and statics").
// A STATIC auto-property's backing storage does not belong to the instance under
// construction, so reading it in the chaining position is legal.
//
// This row exists because the review of this fix found the PropertySymbol arm's
// own `static` carve-out is REDUNDANT: `isAutoPropertyStorageSymbol` already
// refuses to classify a static auto-property as auto-property storage, so the
// carve-out never runs. The behaviour is therefore guaranteed by a DIFFERENT
// mechanism than the code appears to claim -- pin the behaviour here so it
// survives a change to either one.
BEZEL_TEST(chain_position_field_read_3829, static_auto_property_control_accepted) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public static property SP: int := 7; "
        "  private _f: int; "
        "  public constructor(v: int, w: int) "
        "    _f := w; "
        "  end constructor; "
        "  public constructor(v: int) "
        "    this(v, SP); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(_f.ToString()); "
        "  end Show; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    var b := new B(5); "
        "    b.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (r.diagnostics.hasErrors()) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

BEZEL_TEST(chain_position_field_read_3829, literal_control_accepted) {
    auto r = runAot(
        "module M; "
        "class B "
        "  private _f: int; "
        "  public constructor(v: int, w: int) "
        "    _f := w; "
        "  end constructor; "
        "  public constructor(v: int) "
        "    this(v, 42); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(_f.ToString()); "
        "  end Show; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    var b := new B(5); "
        "    b.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (r.diagnostics.hasErrors()) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("42\n"));
}
