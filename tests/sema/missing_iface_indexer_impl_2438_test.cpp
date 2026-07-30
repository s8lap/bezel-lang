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
// "Declared directly on `t`" is deliberate and catches a THIRD, INDEPENDENT
// crash shape found during this investigation: a class satisfying the
// interface obligation only through an INHERITED indexer (declared on a
// base class, never redeclared on the derived class that lists the
// interface) — `findIndexer` never walks `baseClassOf` (unlike its
// named-property sibling `classPropertyByName`, which does), so this
// crashes identically even though the base class's own indexer is
// perfectly correct. Filed as #2442 (a codegen change — teach
// `findIndexer`/`classIndexerImplementing` to walk the base chain,
// mirroring `classPropertyByName`); landing it would need this file's
// `inherited_only_*` reject cases revisited (they would become false
// rejects once the codegen side can actually wire the inherited indexer).
//
// The AOT run oracle is used for the same `rejects()`-is-not-enough reason
// #2417's own test file documents: `runAot` synthesizes an Error diagnostic
// for a runtime SIGSEGV too, so only the diagnostic-TEXT assertion actually
// discriminates pre-fix (crash, diagnostic text absent) from post-fix
// (genuine compile-time reject, diagnostic text present).

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

// ---- REJECT case 3 (satisfaction reached ONLY through inheritance — the
//      independent third crash shape found during this fix) ----------------
const char* kInheritedOnlyReadWriteRejected =
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
    "    return ib[5]; "
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
           inherited_only_satisfaction_rejected_11_2_23_4) {
    BEZEL_EXPECT_TRUE(rejects(kInheritedOnlyReadWriteRejected));
    BEZEL_EXPECT_TRUE(
        hasMissingIndexerImplError(kInheritedOnlyReadWriteRejected));
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
