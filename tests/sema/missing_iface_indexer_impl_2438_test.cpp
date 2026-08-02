// Spec-derived regression test for issue #2438 — a class or record
// implementing an interface indexer obligation while providing NO matching
// indexer of its own at all compiled clean and SIGSEGVd on the GETTER (not
// merely the setter — see #2417's own sibling accessor-set check, whose own
// candidate search this test's fix extends).
//
// Root cause (per the issue, confirmed by this fix's own verify-first):
// #2417's own new `ifaceIndexerObligations` loop in
// `Resolver::checkAbstractImplementations` (§11.2 conformance,
// TypeResolution_Checks.cpp) only ever fired its diagnostic when a
// candidate indexer WAS found on `t` but lacked a setter the interface
// obligation needed — when no candidate existed at all (`candidate ==
// nullptr` after the first-declared fallback), the loop silently
// `continue`d. `declareVtables` Pass D (LlvmBackend_Setup.cpp) calls
// `classIndexerImplementing`, which returns null in that exact shape
// (`findIndexer` finds no indexer member on `t` at all), and BOTH the
// getter and setter itable slots are installed as `ConstantPointerNull`
// unconditionally in that case — `iface[i]` (a plain READ) then calls
// through the null getter pointer.
//
// Why this fix does NOT widen the existing `ifacePropertyNames` /
// `concretePropertyNames` "does not implement interface property" check
// (`TypeResolution_Checks.cpp`, the #489 check, a few hundred lines above
// #2417's own indexer loop) to cover indexers, even though that looks like
// the natural place: EVERY indexer's `PropertySymbol::name` is the same
// synthetic `".this"` (`SymbolCollection.cpp`), independent of parameter
// list — a class with, say, only a `[k: string]` indexer would satisfy a
// name-keyed lookup for an entirely unrelated `[i: int]` interface
// obligation, a false ACCEPT for a genuinely wrong-shaped-only class. This
// fix instead extends #2417's own SHAPE-AWARE candidate search (which
// already mirrors codegen's `classIndexerImplementing`/`findIndexer`
// selection): `candidate == nullptr` there is already the sound "zero
// non-static indexer members of any shape declared directly on `t`"
// condition.
//
// UPDATE (#2442 landed, §11.2/§23.4, v0.235): at the time this file was
// written, the candidate search — mirroring codegen's own
// `classIndexerImplementing`/`findIndexer` — was scoped to `t->members`
// only ("declared directly on `t`"), so a class satisfying the interface
// obligation ONLY through an INHERITED indexer (declared on a base class,
// never redeclared on the derived class that lists the interface) crashed
// identically (`findIndexer` never called `baseClassOf`, unlike its
// named-property sibling `classPropertyByName`), even though the base
// class's own indexer was perfectly correct — filed as #2442. #2442 taught
// both `findIndexer` (codegen) and this file's own mirrored candidate
// search (sema) to walk the base CLASS chain together, so a class-local
// base's indexer now correctly satisfies the obligation and dispatches
// through it. The `inherited_only_*` case below is flipped from a REJECT
// to an ACCEPT (read-AND-write, mirroring #2417's own T2 read/write
// discipline) as part of that same #2442 fix, landed in this file
// alongside the codegen change so the two never went out of sync — see
// #2437/#2442's shared commit for the full "why together" rationale
// (matching codegen's actual itable-wiring capability: an indexer reached
// only through an IMPORTED base class still cannot be wired today, so that
// narrower shape is intentionally left rejected, not attempted here).
//
// The AOT run oracle is used for the same `rejects()`-is-not-enough reason
// #2417's own test file documents: `runAot` synthesizes an Error diagnostic
// for a runtime SIGSEGV too, so only the diagnostic-TEXT assertion actually
// discriminates a genuine compile-time reject (diagnostic text present)
// from a crash masquerading as one (diagnostic text absent).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bool runs(int expected, const std::string& src) {
    auto r = runAot(src);
    return r.ran && r.exitCode == expected;
}

bool rejects(const std::string& src) {
    auto r = runAot(src);
    return !r.ran && r.diagnostics.hasErrors();
}

// True iff an ERROR diagnostic carries #2438's own "does not implement
// interface indexer" text (distinguishes this defect from #2417's own
// "getter-only ... read-write" accessor-set diagnostic, and from #489's
// plain-property "does not implement interface property" wording).
bool hasMissingIndexerImplError(const std::string& src) {
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("23.4") == std::string::npos) continue;
        if (d.message.find("does not implement interface") ==
            std::string::npos)
            continue;
        if (d.message.find("indexer") == std::string::npos) continue;
        return true;
    }
    return false;
}

// ---- REJECT case 1 (the issue's own repro: zero indexer, read-write
//      obligation) ----------------------------------------------------------
const char* kZeroIndexerReadWriteRejected =
    "module M; "
    "public interface IBox "
    "  property [i: int]: int; "  // read-write
    "end IBox; "
    "public class Box: IBox "
    "  private _v: int := 10; "
    "end Box; "
    "public class C "
    "  public static method Main(): int "
    "    using var b := new Box(); "
    "    var ib: IBox := b; "
    "    return ib[0]; "
    "  end Main; "
    "end C;";

// ---- REJECT case 2 (zero indexer, READONLY obligation — proves the check
//      is not gated on the obligation needing a setter; even a getter-only
//      obligation needs SOME candidate) -------------------------------------
const char* kZeroIndexerReadonlyRejected =
    "module M; "
    "public interface IBox "
    "  readonly property [i: int]: int; "
    "end IBox; "
    "public class Box: IBox "
    "  private _v: int := 10; "
    "end Box; "
    "public class C "
    "  public static method Main(): int "
    "    using var b := new Box(); "
    "    var ib: IBox := b; "
    "    return ib[0]; "
    "  end Main; "
    "end C;";

// ---- ACCEPT case (§11.2/§23.4, v0.235, #2442): satisfaction reached ONLY
//      through inheritance (Base declares the indexer; Derived lists the
//      interface without redeclaring it) is now a VALID implementation —
//      #2442 taught `findIndexer`/`classIndexerImplementing` (codegen) and
//      this file's own mirrored candidate search (sema) to walk the base
//      CLASS chain together. Was REJECT case 3 pre-#2442 (the independent
//      third crash shape found during #2438's own fix); flipped here in the
//      same commit that lands #2442, per that fix's own coupling
//      discipline. Exercises BOTH write and read through the interface
//      reference (mirrors #2417's own T2 read/write discipline, and
//      kDirectReadWriteAccepted below) so a fix that only patches the
//      getter path or only the setter path cannot pass by accident:
//      `ib[5] := 42` dispatches through Derived's itable to Base's SETTER
//      (`_v := 42 - 5 = 37`), then `ib[0]` dispatches to Base's GETTER
//      (`_v (37) + 0 = 37`).
const char* kInheritedOnlyReadWriteAccepted =
    "module M; "
    "public interface IBox "
    "  property [i: int]: int; "
    "end IBox; "
    "public class Base "
    "  private _v: int := 10; "
    "  public property [i: int]: int "
    "    get => _v + i; "
    "    set => _v := value - i; "
    "  end; "
    "end Base; "
    "public class Derived: Base, IBox "
    "end Derived; "
    "public class C "
    "  public static method Main(): int "
    "    using var d := new Derived(); "
    "    var ib: IBox := d; "
    "    ib[5] := 42; "        // _v := 37
    "    return ib[0]; "       // 37
    "  end Main; "
    "end C;";

// ---- REJECT case 4 (RECORD, zero indexer at all — a positional field is
//      not an indexer candidate) --------------------------------------------
const char* kRecordZeroIndexerRejected =
    "module M; "
    "public interface IBox "
    "  property [i: int]: int; "
    "end IBox; "
    "public record Box(seed: int): IBox "
    "end Box; "
    "public class C "
    "  public static method Main(): int "
    "    var b := new Box(10); "
    "    var ib: IBox := b; "
    "    return ib[5]; "
    "  end Main; "
    "end C;";

// ---- ACCEPT control 1 (a DIRECT matching read-write indexer — the ordinary
//      case must stay accepted and run correctly) --------------------------
const char* kDirectReadWriteAccepted =
    "module M; "
    "public interface IBox "
    "  property [i: int]: int; "
    "end IBox; "
    "public class Box: IBox "
    "  private _v: int := 10; "
    "  public property [i: int]: int "
    "    get => _v + i; "
    "    set => _v := value - i; "
    "  end; "
    "end Box; "
    "public class C "
    "  public static method Main(): int "
    "    using var b := new Box(); "
    "    var ib: IBox := b; "
    "    ib[5] := 42; "        // _v := 37
    "    return ib[0]; "       // 37
    "  end Main; "
    "end C;";

// ---- ACCEPT control 2 (a DIRECT matching readonly indexer against a
//      readonly obligation) -------------------------------------------------
const char* kDirectReadonlyAccepted =
    "module M; "
    "public interface IBox "
    "  readonly property [i: int]: int; "
    "end IBox; "
    "public class Box: IBox "
    "  private _v: int := 10; "
    "  public property [i: int]: int => _v + i; "
    "end Box; "
    "public class C "
    "  public static method Main(): int "
    "    using var b := new Box(); "
    "    var ib: IBox := b; "
    "    return ib[5]; "  // 15
    "  end Main; "
    "end C;";

}  // namespace

// ---- FAIL-without / PASS-with: no matching indexer at all is REJECTED with
//      #2438's own diagnostic. ---------------------------------------------

BEZEL_TEST(missing_iface_indexer_impl_2438,
           zero_indexer_readwrite_obligation_rejected_11_2_23_4) {
    BEZEL_EXPECT_TRUE(rejects(kZeroIndexerReadWriteRejected));
    BEZEL_EXPECT_TRUE(hasMissingIndexerImplError(kZeroIndexerReadWriteRejected));
}

BEZEL_TEST(missing_iface_indexer_impl_2438,
           zero_indexer_readonly_obligation_rejected_11_2_23_4) {
    BEZEL_EXPECT_TRUE(rejects(kZeroIndexerReadonlyRejected));
    BEZEL_EXPECT_TRUE(hasMissingIndexerImplError(kZeroIndexerReadonlyRejected));
}

BEZEL_TEST(missing_iface_indexer_impl_2438,
           record_zero_indexer_rejected_11_2_23_4) {
    BEZEL_EXPECT_TRUE(rejects(kRecordZeroIndexerRejected));
    BEZEL_EXPECT_TRUE(hasMissingIndexerImplError(kRecordZeroIndexerRejected));
}

// ---- ACCEPT controls (over-rejection safety — must NOT regress) -----------

BEZEL_TEST(missing_iface_indexer_impl_2438,
           direct_readwrite_accepted_runs_correct_value) {
    BEZEL_EXPECT_TRUE(runs(37, kDirectReadWriteAccepted));
}

BEZEL_TEST(missing_iface_indexer_impl_2438,
           direct_readonly_accepted_runs_correct_value) {
    BEZEL_EXPECT_TRUE(runs(15, kDirectReadonlyAccepted));
}

// ---- ACCEPT (fix #2442, §11.2/§23.4, v0.235): satisfaction reached ONLY
//      through inheritance now compiles AND dispatches correctly through
//      BOTH the setter and the getter — see kInheritedOnlyReadWriteAccepted's
//      own comment above for why the shape exercises write-then-read. This
//      is the flip of this file's former `inherited_only_satisfaction_
//      rejected_11_2_23_4` REJECT case, landed in the same commit as #2442
//      itself (a codegen change to `findIndexer`/`classIndexerImplementing`,
//      LlvmBackend_Assign.cpp) per that fix's own coupling discipline: sema
//      and codegen must never disagree about which candidate satisfies an
//      interface indexer obligation.
BEZEL_TEST(missing_iface_indexer_impl_2438,
           inherited_only_satisfaction_accepted_runs_correct_value_2442) {
    BEZEL_EXPECT_TRUE(runs(37, kInheritedOnlyReadWriteAccepted));
}
