// Spec-derived regression test for issue #2413 — a class satisfying a
// read-write interface property with a GETTER-ONLY implementation compiled
// clean and then SIGSEGVd on write through the interface reference. PURELY
// LOCAL (no import, no cross-module boundary) — distinct from the #2374
// `ps->decl`-gate family and from #2406 (a different, cross-module-specific
// null-setter-slot gap for imported properties).
//
// Root cause (per the issue, confirmed by this fix's own verify-first):
// `Resolver::checkAbstractImplementations` (§11.2 conformance,
// TypeResolution_Checks.cpp) never checked that the implementing property's
// ACCESSOR SET (get/set) covers the interface obligation's declared accessor
// set — only the interface's own accessor set gated whether codegen's
// `declareVtables` Pass D (LlvmBackend_Setup.cpp) allocated an itable setter
// slot. A getter-only class property was accepted as satisfying a
// read-write interface property obligation; `ensurePropertyAccessorFn(ts,
// impl, isSet=true)` then correctly returned nullptr for the getter-only
// impl, so Pass D silently installed `ConstantPointerNull` in the
// unfillable setter slot — writing through the interface reference called
// through that null pointer (confirmed via gdb: SIGSEGV at RIP=0x0).
//
// §11.2 (v0.235) states the matching rule and, in its diamond-resolution
// note, the one-directional relaxation explicitly: "A read-write member
// satisfies a read-only interface member of the same name and type" — which,
// by omission, means the converse does NOT hold: a read-only (getter-only)
// member does not satisfy a read-write interface property obligation. §23.3
// (v0.235) is the accessor-set definition ("A read-only property in block
// form omits `set`; a write-only property omits `get`") that the interface
// obligation and the implementation are each read against. This fix adds
// the missing accessor-set check to `checkAbstractImplementations`,
// rejecting the getter-only-vs-read-write mismatch at compile time instead
// of installing a null itable slot.
//
// The AOT run oracle is used so the diagnostic fires through the full
// front-end + AOT path. NOTE on `rejects()`: `runAot` synthesizes an Error
// diagnostic for a runtime SIGSEGV too ("program terminated by signal 11" —
// aot_support.hpp's WIFSIGNALED branch), so `rejects()` alone (!ran &&
// hasErrors()) is satisfied BOTH by a genuine compile-time reject (post-fix)
// AND by the pre-fix crash-through-null-itable-slot shape (compiles clean,
// SIGSEGVs at runtime) — it is kept as a coarse sanity check but is NOT the
// fail-without/pass-with pin by itself. `hasGetterOnlyVsReadWriteError` (the
// specific diagnostic TEXT) is the assertion that actually discriminates the
// two states — confirmed mechanically: reverting just the sema fix (`git
// checkout -- src/sema/TypeResolution_Checks.cpp`) makes ONLY the
// `hasGetterOnlyVsReadWriteError` assertions fail (rejects() keeps passing,
// for the wrong/crash reason); reapplying the fix makes both pass for the
// right reason. ACCEPT controls assert `ran && exitCode == expected`.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Compile-and-run via the AOT pipeline; returns true iff the program
// compiled, linked, and ran to normal termination with the expected exit
// status.
bool runs(int expected, const std::string& src) {
    auto r = runAot(src);
    return r.ran && r.exitCode == expected;
}

// Compile-and-run via the AOT pipeline; returns true iff the program was
// REJECTED at compile time with at least one error diagnostic.
bool rejects(const std::string& src) {
    auto r = runAot(src);
    return !r.ran && r.diagnostics.hasErrors();
}

// True iff an ERROR diagnostic carries #2413's own getter-only-vs-read-write
// accessor-set-mismatch text — pins THIS defect specifically (distinguishes
// it from #489's "field does not satisfy a property contract" and #2380's
// "different type than the interface declares" diagnostics, which share the
// same §11.2 citation but different wording).
bool hasGetterOnlyVsReadWriteError(const std::string& src) {
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("11.2") == std::string::npos) continue;
        if (d.message.find("getter-only") == std::string::npos) continue;
        if (d.message.find("read-write") == std::string::npos) continue;
        if (d.message.find("setter") == std::string::npos) continue;
        return true;
    }
    return false;
}

// ---- REJECT case 1 (the issue's own repro, expression-bodied) -------------
// IHasBalance.Balance is read-write (no `readonly`); GetterOnlyImpl provides
// only a getter (expression-bodied). FAIL-WITHOUT: compiles clean and
// SIGSEGVs on `b.Balance := 2.0m` (confirmed via gdb, RIP=0x0);
// PASS-WITH: rejected at the conformance check.
const char* kExprBodiedGetterOnlyRejected =
    "module M; "
    "public interface IHasBalance "
    "  property Balance: decimal; "
    "end IHasBalance; "
    "public class GetterOnlyImpl: IHasBalance "
    "  public property Balance: decimal => 1.0m; "
    "end GetterOnlyImpl; "
    "public class C "
    "  public static method Main(): int "
    "    using var b: IHasBalance := new GetterOnlyImpl(); "
    "    b.Balance := 2.0m; "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---- REJECT case 2 (block-form getter-only — no `set` arm) ----------------
// Same shape as case 1, but the implementation uses block-form syntax
// (§23.3) with only a `get` accessor rather than an expression body. Exercises
// the `propertyDeclHasSetter`'s Block-body branch (accessors list has no
// `isSet` entry), not just its Expression-body branch.
const char* kBlockFormGetterOnlyRejected =
    "module M; "
    "public interface IHasBalance "
    "  property Balance: decimal; "
    "end IHasBalance; "
    "public class GetterOnlyImpl: IHasBalance "
    "  private _balance: decimal := 1.0m; "
    "  public property Balance: decimal "
    "    get => _balance; "
    "  end Balance; "
    "end GetterOnlyImpl; "
    "public class C "
    "  public static method Main(): int "
    "    using var b: IHasBalance := new GetterOnlyImpl(); "
    "    b.Balance := 2.0m; "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---- ACCEPT control 1 (getter-only satisfying a getter-only/readonly
//      interface property — must STAY legal) -------------------------------
// IHasBalance.Balance is declared `readonly` — a getter-only implementation
// fully satisfies the read-only obligation. Reads 1.0m through the
// interface reference and returns a distinguishing exit code.
const char* kGetterOnlyVsReadonlyAccepted =
    "module M; "
    "public interface IHasBalance "
    "  readonly property Balance: decimal; "
    "end IHasBalance; "
    "public class GetterOnlyImpl: IHasBalance "
    "  public property Balance: decimal => 1.0m; "
    "end GetterOnlyImpl; "
    "public class C "
    "  public static method Main(): int "
    "    using var b: IHasBalance := new GetterOnlyImpl(); "
    "    if b.Balance = 1.0m then "
    "      return 42; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---- ACCEPT control 2 (read-write satisfying read-write — control) --------
// A full read-write implementation (block-form get/set) of a read-write
// interface property. Writes 2.0m through the interface reference and reads
// it back — the itable setter slot is genuinely filled, so this must both
// compile AND run to a correct value (not merely compile).
const char* kReadWriteVsReadWriteAccepted =
    "module M; "
    "public interface IHasBalance "
    "  property Balance: decimal; "
    "end IHasBalance; "
    "public class RwImpl: IHasBalance "
    "  private _balance: decimal := 1.0m; "
    "  public property Balance: decimal "
    "    get => _balance; "
    "    set => do "
    "      _balance := value; "
    "    end; "
    "  end Balance; "
    "end RwImpl; "
    "public class C "
    "  public static method Main(): int "
    "    using var b: IHasBalance := new RwImpl(); "
    "    b.Balance := 2.0m; "
    "    if b.Balance = 2.0m then "
    "      return 42; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---- ACCEPT control 3 (read-write satisfying a readonly obligation) -------
// §11.2 (v0.235) states this explicitly: "A read-write member satisfies a
// read-only interface member of the same name and type." Must STAY
// accepted — the fix's directional check (interface requires a setter AND
// impl lacks one) must not misfire on the opposite, always-legal direction.
const char* kReadWriteVsReadonlyAccepted =
    "module M; "
    "public interface IHasBalance "
    "  readonly property Balance: decimal; "
    "end IHasBalance; "
    "public class RwImpl: IHasBalance "
    "  private _balance: decimal := 1.0m; "
    "  public property Balance: decimal "
    "    get => _balance; "
    "    set => do "
    "      _balance := value; "
    "    end; "
    "  end Balance; "
    "end RwImpl; "
    "public class C "
    "  public static method Main(): int "
    "    using var b: IHasBalance := new RwImpl(); "
    "    if b.Balance = 1.0m then "
    "      return 42; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// ---- FAIL-without / PASS-with: getter-only vs read-write is REJECTED with
//      #2413's own diagnostic. --------------------------------------------

// (1) The issue's repro: expression-bodied getter-only implementation.
// Pre-fix this compiles and SIGSEGVs on write; post-fix it is rejected with
// the accessor-set-mismatch diagnostic.
BEZEL_TEST(getter_only_readwrite_iface_property_2413,
           expr_bodied_getter_only_vs_readwrite_rejected_11_2) {
    BEZEL_EXPECT_TRUE(rejects(kExprBodiedGetterOnlyRejected));
    BEZEL_EXPECT_TRUE(hasGetterOnlyVsReadWriteError(kExprBodiedGetterOnlyRejected));
}

// (2) Same shape, block-form (`get => …;` with no `set` arm). Same
// fail-without / pass-with.
BEZEL_TEST(getter_only_readwrite_iface_property_2413,
           block_form_getter_only_vs_readwrite_rejected_11_2) {
    BEZEL_EXPECT_TRUE(rejects(kBlockFormGetterOnlyRejected));
    BEZEL_EXPECT_TRUE(hasGetterOnlyVsReadWriteError(kBlockFormGetterOnlyRejected));
}

// ---- ACCEPT controls (over-rejection safety — these must NOT regress) -----

// (3) Getter-only implementation of a `readonly` interface property property
// stays legal, compiles, and RUNS to the correct value (not merely compiles).
BEZEL_TEST(getter_only_readwrite_iface_property_2413,
           getter_only_vs_readonly_accepted_runs_correct_value) {
    BEZEL_EXPECT_TRUE(runs(42, kGetterOnlyVsReadonlyAccepted));
}

// (4) Read-write implementation of a read-write interface property (control)
// stays legal; writes and reads back the correct value through the
// interface reference — the itable setter slot is genuinely exercised.
BEZEL_TEST(getter_only_readwrite_iface_property_2413,
           readwrite_vs_readwrite_accepted_runs_correct_value) {
    BEZEL_EXPECT_TRUE(runs(42, kReadWriteVsReadWriteAccepted));
}

// (5) Read-write implementation of a `readonly` interface property stays
// legal per §11.2's explicit one-directional relaxation ("a read-write
// member satisfies a read-only interface member").
BEZEL_TEST(getter_only_readwrite_iface_property_2413,
           readwrite_vs_readonly_accepted_runs_correct_value) {
    BEZEL_EXPECT_TRUE(runs(42, kReadWriteVsReadonlyAccepted));
}
