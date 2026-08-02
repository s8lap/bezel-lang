// Spec-derived regression test for issue #2417 — the INDEXER sibling of
// #2413: a class (or record) satisfying a read-write interface INDEXER with
// a getter-only implementation compiled clean and then SIGSEGVd on write
// through the interface reference. Mirrors
// getter_only_readwrite_iface_property_2413_test.cpp's structure exactly,
// adapted for indexers' distinguishing shape (anonymous, overloadable by
// parameter type list, §23.4) plus the axes that shape adds: overloaded
// indexers, a generic interface indexer, and records.
//
// Root cause (per the issue, confirmed by this fix's own verify-first):
// `Resolver::checkAbstractImplementations` (§11.2 conformance,
// TypeResolution_Checks.cpp) explicitly skipped every indexer
// (`if (ps->isIndexer) continue; // indexers are separate`) — no accessor-set
// check ran for them at all. `declareVtables` Pass D (LlvmBackend_Setup.cpp,
// the `ip->isIndexer` branch) installs `ConstantPointerNull` in the
// interface indexer's setter itable slot whenever the matched concrete
// indexer has no setter, with no diagnostic — `iface[i] := v` then calls
// through that null pointer (SIGSEGV, confirmed via the issue's own gdb
// trace: RIP=0x0).
//
// §11.2 (v0.235) states the matching rule and, in its diamond-resolution
// note, the one-directional relaxation explicitly: "A read-write member
// satisfies a read-only interface member of the same name and type" — which,
// by omission, means the converse does NOT hold. §23.4 (v0.235) is the
// indexer definition ("An indexer is a property whose name is replaced by
// `[parameters]`" — the same accessor-set contract as §23.3's plain
// properties) and states indexers "may be overloaded by parameter type
// list" and that "Body-form records with mutable properties may declare
// read-write indexers; positional-only records may not."
//
// The AOT run oracle is used so the diagnostic fires through the full
// front-end + AOT path. NOTE on `rejects()`: `runAot` synthesizes an Error
// diagnostic for a runtime SIGSEGV too, so `rejects()` alone is satisfied
// BOTH by a genuine compile-time reject (post-fix) AND by the pre-fix
// crash-through-null-itable-slot shape (the program compiles clean and
// SIGSEGVs at `ib[5] := 42`; `runAot`'s WIFSIGNALED branch then synthesizes
// an Error diagnostic for the signal) — `rejects()` is kept as a coarse
// sanity check but is NOT the fail-without/pass-with pin by itself.
// `hasGetterOnlyVsReadWriteIndexerError` (the specific diagnostic TEXT) is
// the assertion that actually discriminates the two states — confirmed
// mechanically: reverting just the sema fix (`git checkout --
// src/sema/TypeResolution_Checks.cpp`) leaves all four `rejects()`
// assertions passing (the crash still synthesizes an Error) but flips all
// four `hasGetterOnlyVsReadWriteIndexerError` assertions to failing, while
// every ACCEPT-control test is unaffected — reproduced for this exact file
// via `agent_check.sh getter_only_readwrite_iface_indexer_2417`: 6
// passed / 4 failed pre-fix (the 4 REJECT cases, on the diagnostic-text
// assertion only), 10/10 post-fix. ACCEPT controls assert
// `ran && exitCode == expected`.

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

// True iff an ERROR diagnostic carries #2417's own getter-only-vs-read-write
// indexer accessor-set-mismatch text.
bool hasGetterOnlyVsReadWriteIndexerError(const std::string& src) {
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("23.4") == std::string::npos) continue;
        if (d.message.find("indexer") == std::string::npos) continue;
        if (d.message.find("getter-only") == std::string::npos) continue;
        if (d.message.find("read-write") == std::string::npos) continue;
        if (d.message.find("setter") == std::string::npos) continue;
        return true;
    }
    return false;
}

// ---- REJECT case 1 (the issue's own repro, expression-bodied) -------------
const char* kExprBodiedGetterOnlyRejected =
    "module M; "
    "public interface IBox "
    "  property [i: int]: int; "  // read-write (no `readonly`)
    "end IBox; "
    "public class Box: IBox "
    "  private _v: int := 10; "
    "  public property [i: int]: int => _v + i; "  // getter-only
    "end Box; "
    "public class C "
    "  public static method Main(): int "
    "    using var b := new Box(); "
    "    var ib: IBox := b; "
    "    ib[5] := 42; "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---- REJECT case 2 (block-form getter-only — no `set` arm) ----------------
const char* kBlockFormGetterOnlyRejected =
    "module M; "
    "public interface IBox "
    "  property [i: int]: int; "
    "end IBox; "
    "public class Box: IBox "
    "  private _v: int := 10; "
    "  public property [i: int]: int "
    "    get => _v + i; "
    "  end; "
    "end Box; "
    "public class C "
    "  public static method Main(): int "
    "    using var b := new Box(); "
    "    var ib: IBox := b; "
    "    ib[5] := 42; "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---- ACCEPT control 1 (getter-only satisfying a readonly interface
//      indexer — must STAY legal) ------------------------------------------
const char* kGetterOnlyVsReadonlyAccepted =
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

// ---- ACCEPT control 2 (read-write satisfying read-write) — the itable
//      setter slot is genuinely filled and exercised. -----------------------
const char* kReadWriteVsReadWriteAccepted =
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
    "    ib[5] := 42; "        // _v := 42 - 5 = 37
    "    return ib[0]; "       // 37
    "  end Main; "
    "end C;";

// ---- ACCEPT control 3 (read-write satisfying a readonly obligation) -------
// §11.2's explicit one-directional relaxation.
const char* kReadWriteVsReadonlyAccepted =
    "module M; "
    "public interface IBox "
    "  readonly property [i: int]: int; "
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
    "    return ib[5]; "  // 15
    "  end Main; "
    "end C;";

// ---- ACCEPT control 4 (OVERLOADED indexers — false-reject safety) ---------
// §23.4: "Indexers may be overloaded by parameter type list." `Box` declares
// TWO indexers: an `int`-keyed one (read-write, matches IBox's obligation)
// and a `string`-keyed one (getter-only, irrelevant to IBox). The
// getter-only string overload must not spuriously trip the accessor-set
// check for the (unrelated) matching int overload.
const char* kOverloadedIndexerAccepted =
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
    "  public property [k: string]: int => _v + k.Length; "
    "end Box; "
    "public class C "
    "  public static method Main(): int "
    "    using var b := new Box(); "
    "    var ib: IBox := b; "
    "    ib[5] := 42; "        // _v := 37
    "    return ib[0]; "       // 37
    "  end Main; "
    "end C;";

// ---- REJECT case 3 (GENERIC interface indexer, getter-only impl) ----------
// Accessor PRESENCE is a shape fact, not a typed one — substituting the
// interface's type parameter does not change which accessors it declares,
// so the check must fire under a generic interface too.
const char* kGenericInterfaceGetterOnlyRejected =
    "module M; "
    "public interface IBag<T> "
    "  property [i: int]: T; "
    "end IBag; "
    "public class Box<T>: IBag<T> "
    "  private _v: T; "
    "  public constructor(v: T) _v := v; end constructor; "
    "  public property [i: int]: T => _v; "  // getter-only
    "end Box; "
    "public class C "
    "  public static method Main(): int "
    "    using var b := new Box<int>(10); "
    "    var ib: IBag<int> := b; "
    "    ib[0] := 55; "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---- ACCEPT control 5 (GENERIC interface indexer, real read-write impl) ---
const char* kGenericInterfaceReadWriteAccepted =
    "module M; "
    "public interface IBag<T> "
    "  property [i: int]: T; "
    "end IBag; "
    "public class Box<T>: IBag<T> "
    "  private _v: T; "
    "  public constructor(v: T) _v := v; end constructor; "
    "  public property [i: int]: T "
    "    get => _v; "
    "    set => _v := value; "
    "  end; "
    "end Box; "
    "public class C "
    "  public static method Main(): int "
    "    using var b := new Box<int>(10); "
    "    var ib: IBag<int> := b; "
    "    ib[0] := 55; "
    "    return ib[0]; "  // 55
    "  end Main; "
    "end C;";

// ---- REJECT case 4 (RECORD, positional-only — forced getter-only) ---------
// §23.4: a positional-only record may not declare a read-write indexer at
// all, so ANY record indexer implementing a read-write interface obligation
// via a positional-record property is necessarily getter-only. Confirms
// #2417's check applies uniformly to records (this function's outer loop
// already admits Record, #2416) with no record-specific matching code.
const char* kRecordGetterOnlyRejected =
    "module M; "
    "public interface IBox "
    "  property [i: int]: int; "
    "end IBox; "
    "public record Box(seed: int): IBox "
    "  public property [i: int]: int => seed + i; "
    "end Box; "
    "public class C "
    "  public static method Main(): int "
    "    var b := new Box(10); "
    "    var ib: IBox := b; "
    "    ib[5] := 42; "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---- ACCEPT control 6 (RECORD, body-form with a mutable property — real
//      read-write indexer, §23.4 "Body-form records with mutable properties
//      may declare read-write indexers") ------------------------------------
const char* kRecordBodyFormReadWriteAccepted =
    "module M; "
    "public interface IBox "
    "  property [i: int]: int; "
    "end IBox; "
    "public record Box: IBox "
    "  private _v: int; "
    "  public constructor(seed: int) _v := seed; end constructor; "
    "  public property [i: int]: int "
    "    get => _v + i; "
    "    set => _v := value - i; "
    "  end; "
    "end Box; "
    "public class C "
    "  public static method Main(): int "
    "    var b := new Box(10); "
    "    var ib: IBox := b; "
    "    ib[5] := 42; "        // _v := 37
    "    return ib[0]; "       // 37
    "  end Main; "
    "end C;";

}  // namespace

// ---- FAIL-without / PASS-with: getter-only vs read-write is REJECTED with
//      #2417's own diagnostic. --------------------------------------------

BEZEL_TEST(getter_only_readwrite_iface_indexer_2417,
           expr_bodied_getter_only_vs_readwrite_rejected_11_2_23_4) {
    BEZEL_EXPECT_TRUE(rejects(kExprBodiedGetterOnlyRejected));
    BEZEL_EXPECT_TRUE(
        hasGetterOnlyVsReadWriteIndexerError(kExprBodiedGetterOnlyRejected));
}

BEZEL_TEST(getter_only_readwrite_iface_indexer_2417,
           block_form_getter_only_vs_readwrite_rejected_11_2_23_4) {
    BEZEL_EXPECT_TRUE(rejects(kBlockFormGetterOnlyRejected));
    BEZEL_EXPECT_TRUE(
        hasGetterOnlyVsReadWriteIndexerError(kBlockFormGetterOnlyRejected));
}

BEZEL_TEST(getter_only_readwrite_iface_indexer_2417,
           generic_iface_getter_only_vs_readwrite_rejected_11_2_23_4) {
    BEZEL_EXPECT_TRUE(rejects(kGenericInterfaceGetterOnlyRejected));
    BEZEL_EXPECT_TRUE(hasGetterOnlyVsReadWriteIndexerError(
        kGenericInterfaceGetterOnlyRejected));
}

BEZEL_TEST(getter_only_readwrite_iface_indexer_2417,
           record_positional_getter_only_vs_readwrite_rejected_11_2_23_4) {
    BEZEL_EXPECT_TRUE(rejects(kRecordGetterOnlyRejected));
    BEZEL_EXPECT_TRUE(
        hasGetterOnlyVsReadWriteIndexerError(kRecordGetterOnlyRejected));
}

// ---- ACCEPT controls (over-rejection safety — these must NOT regress) -----

BEZEL_TEST(getter_only_readwrite_iface_indexer_2417,
           getter_only_vs_readonly_accepted_runs_correct_value) {
    BEZEL_EXPECT_TRUE(runs(15, kGetterOnlyVsReadonlyAccepted));
}

BEZEL_TEST(getter_only_readwrite_iface_indexer_2417,
           readwrite_vs_readwrite_accepted_runs_correct_value) {
    BEZEL_EXPECT_TRUE(runs(37, kReadWriteVsReadWriteAccepted));
}

BEZEL_TEST(getter_only_readwrite_iface_indexer_2417,
           readwrite_vs_readonly_accepted_runs_correct_value) {
    BEZEL_EXPECT_TRUE(runs(15, kReadWriteVsReadonlyAccepted));
}

BEZEL_TEST(getter_only_readwrite_iface_indexer_2417,
           overloaded_indexer_accepted_runs_correct_value) {
    BEZEL_EXPECT_TRUE(runs(37, kOverloadedIndexerAccepted));
}

BEZEL_TEST(getter_only_readwrite_iface_indexer_2417,
           generic_iface_readwrite_accepted_runs_correct_value) {
    BEZEL_EXPECT_TRUE(runs(55, kGenericInterfaceReadWriteAccepted));
}

BEZEL_TEST(getter_only_readwrite_iface_indexer_2417,
           record_bodyform_readwrite_accepted_runs_correct_value) {
    BEZEL_EXPECT_TRUE(runs(37, kRecordBodyFormReadWriteAccepted));
}
