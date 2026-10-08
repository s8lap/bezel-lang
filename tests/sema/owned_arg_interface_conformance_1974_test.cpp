// Spec-derived tests for issue #1974 — an `owned`-MARKED (or `scoped`-MARKED)
// call argument must run the SAME §5.1 implicit-conversion (interface-
// conformance) check that a bare (unmodified) argument runs.
//
// §5.1 (implicit conversions) + §22.3 (ownership transfer at a call): passing a
// value into an interface-typed parameter — whether as a borrow, an `owned`
// ownership transfer, or a `scoped` borrow — requires the source's concrete type
// to IMPLEMENT that interface. §5.1 admits a class→implemented-interface upcast
// and a derived→base upcast as implicit; it does NOT admit a class that fails to
// implement the interface.
//
// Before this fix, checkArgsAgainstParams (ExprTypecheck_Calls.cpp) skipped the
// per-argument checkAssignable for EVERY modified argument (`if (!a.modifier
// .empty()) continue;`). The by-reference family (`ref`/`out`/…) is type-matched
// by its own rules and rightly skipped — but `owned` and `scoped` still pass a
// VALUE, so skipping them let a non-implementing class transfer/borrow into an
// interface parameter compile clean and then dispatch through a null vtable →
// runtime SEGV (the reviewer's ASan repro, #1974). The bare-argument counterpart
// was already correctly rejected here. The fix skips only the by-ref/out family,
// so `owned`/`scoped` now run the same §5.1 conformance check.
//
// runAot returns the FULL int from Main() (not 8-bit wrapped).

#include <string>
#include <string_view>

#include "support/aot_support.hpp"        // runAot — AOT run oracle
#include "support/ownership_support.hpp"  // parseAndCheckOwnership — sema oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

// The §5.1 argument-convertibility diagnostic (checkAssignable) —
// "... is not implicitly convertible to the target type (§5.1)".
bool hasConvertError(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not implicitly convertible") != std::string::npos &&
            d.message.find("5.1") != std::string::npos)
            return true;
    return false;
}

// A `Cell` that does NOT implement `IThing`, plus the interface and a `Take`
// method whose interface parameter modifier the caller varies. `PARAM_MOD` is
// the callee's parameter modifier ("owned " or ""); `ARG_MOD` the call-site
// marker ("owned " / "scoped " / "").
std::string src(std::string_view paramMod, std::string_view argMod,
                std::string_view local) {
    return std::string(
               "module M; "
               "interface IThing method Ping(): int; end IThing; "
               "class Cell "
               "  private _v: int; "
               "  public constructor(v: int) _v := v; end constructor; "
               "end Cell; "
               "class C "
               "  public static method Take(") +
           std::string(paramMod) +
           "x: IThing): int return x.Ping(); end Take; "
           "  public static method Main(): int " +
           std::string(local) +
           "    return C.Take(" + std::string(argMod) +
           "w); "
           "  end Main; "
           "end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// REJECT: a non-implementing class into an interface parameter is rejected for
// EVERY argument form — the fix makes the `owned`/`scoped` markers match the
// already-correct bare form.
// ---------------------------------------------------------------------------

// §5.1 / §22.3 (#1974): the BUG. `C.Take(owned w)` transfers a `Cell` (which
// does not implement `IThing`) into an `owned x: IThing` parameter. Fail-without:
// compiles clean and SEGVs at runtime; pass-with: rejected with the §5.1
// diagnostic, exactly as the bare form below.
BEZEL_TEST(owned_arg_conformance_1974, owned_nonconforming_to_interface_rejected_5_1) {
    auto r = parseAndCheckOwnership(
        src(/*paramMod=*/"owned ", /*argMod=*/"owned ",
            /*local=*/"var w := new Cell(7); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConvertError(r));
}

// §5.1 / §22.3 (#1974): the `scoped` borrow marker has the identical hole — the
// same code line skipped it. `C.Take(scoped w)` borrowing a non-implementing
// `Cell` into an `IThing` parameter is rejected with the §5.1 diagnostic.
BEZEL_TEST(owned_arg_conformance_1974, scoped_nonconforming_to_interface_rejected_5_1) {
    auto r = parseAndCheckOwnership(
        src(/*paramMod=*/"", /*argMod=*/"scoped ",
            /*local=*/"using var w := new Cell(7); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConvertError(r));
}

// §5.1 (#1974): the BARE (unmodified) argument — the already-correct control the
// fix mirrors. `C.Take(w)` passing a non-implementing `Cell` into an `IThing`
// parameter was always rejected; this anchors that behavior is unchanged.
BEZEL_TEST(owned_arg_conformance_1974, bare_nonconforming_to_interface_rejected_5_1) {
    auto r = parseAndCheckOwnership(
        src(/*paramMod=*/"", /*argMod=*/"",
            /*local=*/"var w := new Cell(7); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConvertError(r));
}

// ---------------------------------------------------------------------------
// ACCEPT (sema): the fix must NOT over-reject a legitimate transfer. §5.1 admits
// a class→implemented-interface upcast, an exact class match, and a derived→base
// upcast — each still compiles clean.
// ---------------------------------------------------------------------------

// §5.1 / §22.3 (#1974): `owned` transfer of a class that DOES implement the
// interface is an admitted §5.1 upcast — accepted, no error.
BEZEL_TEST(owned_arg_conformance_1974, owned_conforming_to_interface_accepted_5_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "interface IThing method Ping(): int; end IThing; "
        "class Widget: IThing "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method Ping(): int => _v; "
        "end Widget; "
        "class C "
        "  public static method Take(owned x: IThing): int return x.Ping(); end Take; "
        "  public static method Main(): int "
        "    var w := new Widget(42); "
        "    return C.Take(move w); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §5.1 / §22.3 (#1974): `owned` transfer into a parameter of the EXACT class type
// (identity, not an interface) is accepted — the check keys on §5.1
// convertibility, so a matching class type passes.
BEZEL_TEST(owned_arg_conformance_1974, owned_to_matching_class_param_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Widget "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method Ping(): int => _v; "
        "end Widget; "
        "class C "
        "  public static method Take(owned x: Widget): int return x.Ping(); end Take; "
        "  public static method Main(): int "
        "    var w := new Widget(42); "
        "    return C.Take(move w); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §5.1 / §22.3 (#1974): `owned` transfer of a DERIVED class into a base-class
// parameter is an admitted §5.1 upcast — accepted (no over-rejection of a
// covariant reference conversion).
BEZEL_TEST(owned_arg_conformance_1974, owned_derived_to_base_param_accepted_5_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Base "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public virtual method Ping(): int => _v; "
        "end Base; "
        "class Derived: Base "
        "  public constructor() base(7); end constructor; "
        "end Derived; "
        "class C "
        "  public static method Take(owned x: Base): int return x.Ping(); end Take; "
        "  public static method Main(): int "
        "    var d := new Derived(); "
        "    return C.Take(move d); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// RUN (AOT): the accepted transfers actually execute and dispatch correctly —
// the counterpart to the SEGV the reject prevents. A conforming `owned` transfer
// dispatches `Ping()` through the interface itable and returns 42.
// ---------------------------------------------------------------------------

// §5.1 / §22.3 (#1974): the conforming owned-transfer-to-interface case RUNS —
// `Ping()` dispatches through the real itable → 42 (no SEGV).
BEZEL_TEST(owned_arg_conformance_1974, owned_conforming_to_interface_runs) {
    auto r = runAot(
        "module M; "
        "interface IThing method Ping(): int; end IThing; "
        "class Widget: IThing "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method Ping(): int => _v; "
        "end Widget; "
        "class C "
        "  public static method Take(owned x: IThing): int return x.Ping(); end Take; "
        "  public static method Main(): int "
        "    var w := new Widget(42); "
        "    return C.Take(move w); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// §5.1 / §22.3 (#1974): the exact-class owned-transfer case RUNS → 42.
BEZEL_TEST(owned_arg_conformance_1974, owned_to_matching_class_param_runs) {
    auto r = runAot(
        "module M; "
        "class Widget "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method Ping(): int => _v; "
        "end Widget; "
        "class C "
        "  public static method Take(owned x: Widget): int return x.Ping(); end Take; "
        "  public static method Main(): int "
        "    var w := new Widget(42); "
        "    return C.Take(move w); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}
