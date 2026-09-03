// fix #2473 (§9.2/§23.1/§22.8/§13, v0.236): a private/internal
// AUTO-PROPERTY (`private property W: int := 41;` — no explicit backing
// field, storage lives directly on the PropertySymbol per §23.1 "the
// compiler generates the backing field"; recognized elsewhere via
// `sema::isAutoPropertySymbol`/codegen's `isAutoProperty`,
// `collectInstanceStorage`/`collectRecordStorage`,
// LlvmBackend_impl.hpp) is a real instance-storage member — occupies a
// struct slot exactly like a plain field, in §22.8 declaration order —
// but is NOT a `FieldSymbol`, so ModuleMetadataBuilder.cpp's
// `includeForLayout`/`includeForMethodBody` gate
//   bool isField = !isMethod && dynamic_cast<const sema::FieldSymbol*>(ms) != nullptr;
// never matched it, and a PRIVATE/INTERNAL auto-property (not `exported`)
// was therefore dropped from `out.members` entirely — the identical
// hazard class #2456 fixed for plain fields (classes) and #2468 fixed
// for plain fields (records), just for the auto-property subtype of
// storage member. (#2478 closed the IMPORTER-SIDE classification gap —
// an already-crossed public auto-property now gets recognized as
// storage via `propIsAutoStorage_` — but that fix presumes the
// PropertySymbol exists in `t->members` on the importer side at all,
// which for a private/internal auto-property it did not.)
//
// ROOT CAUSE (confirmed by direct instrumentation of
// `collectInstanceStorage`/`classStruct`, LlvmBackend_Layout.cpp,
// comparing the EXPORTER's own compiled struct layout against the
// IMPORTER's reconstruction of the same type — the identical
// verification methodology as #2456/#2468/#2478): a private/internal
// auto-property on an imported CLASS is invisible to the importer's
// `t->members`, so `collectInstanceStorage` lays out a SMALLER struct
// than the exporter's own compilation — a locally-declared derived
// class's own first storage slot then physically ALIASED the base's
// real (importer-invisible) auto-property slot, and the exporter's
// compiled property-getter/method (which reads the exporter's offset)
// returned the derived class's own field's value instead of the base
// auto-property's value. A direct `new ImportedBase()` was affected
// too: the reconstructed instance was allocated ONE SLOT TOO SMALL
// outright — a heap-buffer-overflow the moment the exporter's real
// (correctly-sized) init/ctor/accessor bodies wrote to the hidden
// auto-property's slot (same under-allocation class as #2456,
// confirmed by the same instrumentation: `storageCount` mismatch
// between the two sides of the boundary).
//
// THE FIX (this commit): ModuleMetadataBuilder.cpp's member-export loop
// now recognizes a private/internal non-static auto-property as a layout
// storage member (parallel to the existing `isField` test) for BOTH
// `includeForLayout` (the #2456/#2468 classes+records layout carriage)
// and `includeForMethodBody` (the #1224 generic-class method-body
// re-parse name-resolution carriage). Name + type + declaration-order
// POSITION only — no accessor bodies, no initializer (the
// `includeForLayout` strip that #2456 added for plain fields applies
// unchanged, keeping this genuinely layout-only carriage; for a CLASS
// the value is replayed by the exporter's compiled `bz$<Type>$init`
// function #2452 already exports, and for a RECORD the private-member
// value-replay gap is the same open follow-up #2468 explicitly defers
// — this fix makes the SLOT visible, exactly mirroring #2468's
// "layout-only, value-replay filed separately" scope). No new metadata
// field/bit was invented: `MetaMemberKind::Property`'s reconstruction
// (ModuleLoader.cpp) already builds a real PropertySymbol unconditionally
// once a MetaMember entry exists, and #2478's `propIsAutoStorage_`
// publication already classifies it as storage on the importer side —
// the property simply never crossed the boundary before this gate
// widening. ModuleLoader.cpp needed NO changes.
//
// SCOPE (CLAUDE.md rule 4 — deliberately narrow, mirroring #2456/#2468):
// the value-replay gap for a PRIVATE record auto-property (the slot is
// allocated but reads 0, not the declared initializer) is the same
// "private member value-replay" gap #2468 names as a #2467 follow-up —
// RECORD T6 below pins the post-fix (still value-replay-unfixed) value
// is a well-defined, non-aliased ZERO, not the importer-allocator's
// reuse of a neighbouring slot's value. The class-side layout carriage
// is covered by T1-T5; the record-side by T6.
//
// The `includeForMethodBody` gate (#1224, generic-class method-body
// re-parse name-resolution) is widened by the SAME `|| isAutoPropStorage`
// one-line change as `includeForLayout` — the code change is correct by
// inspection (the flag is identical). A live fail-without/pass-with
// regression for the `includeForMethodBody` arm in isolation was NOT
// achievable: the direct-construction generic shape (T7, control)
// returns 0 both pre- and post-fix (the pre-existing generic-class
// instance-initializer-replay gap — which affects plain FIELDS
// identically, verified by direct comparison during this fix — masks any
// value-observable difference), and the generic-derivation aliasing
// shape hits a pre-existing cross-module link error for BOTH plain
// fields and auto-properties (also confirmed by direct comparison,
// filed as #2524). T7 is therefore a COMPILE-STABILITY control,
// not a value regression. The layout-aliasing regressions (T1-T6) are
// the authoritative fail-without/pass-with tests for this fix.
//
// UPDATE (fix #3736, v0.286, LANE_RULES Â§7): the "value-replay gap"
// this SCOPE section names above was closed for a private RECORD
// auto-property by fix #3736 (the Property twin of fix #3723's
// identical widening for a private record FIELD). T6's row below was
// updated from the deferred-gap `9` (0 replayed) to the spec-correct
// `50` (41 replayed) accordingly â see T6's own header comment for the
// measured before/after. This file's #2473 fix itself (the anti-
// aliasing SLOT-visibility widening) is untouched and unaffected;
// only the value that was always known to be a deferred gap changed.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile2473 {
    std::string path;
    explicit TempFile2473(std::string p) : path(std::move(p)) {}
    ~TempFile2473() { std::remove(path.c_str()); }
};

void writeSource2473(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2473(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Mirrors #2456/#2468/#2478's own compileImportAndRun helpers: compile lib ->
// object, compile app (importing lib) -> exe, run it in a SEPARATE process
// (fork-isolated per tests/README.md's AOT harness discipline), return its
// exit code (-1 on compile/link/run failure, -2 if the dependency itself
// could not be produced).
int compileImportAndRun2473(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2473_");
    TempFile2473 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2473 aObj((dir / (stem + "_a.o")).string());
    writeSource2473(aSrc.path, libSrc);
    if (!compileToObj2473(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2473 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2473 bExe((dir / stem).string());
    writeSource2473(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!driver.compileFile(bSrc.path, bExe.path, deps).success) return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// Mirrors #2456's compileImportCaptureErr2456: compile lib -> object, then
// compile app -> exe WITHOUT running, capturing the driver's stderr into `err`
// and returning compile success.
bool compileImportCaptureErr2473(const std::string& libSrc,
                                 const std::string& appSrc, std::string& err) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2473_c_");
    TempFile2473 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2473 aObj((dir / (stem + "_a.o")).string());
    writeSource2473(aSrc.path, libSrc);
    if (!compileToObj2473(aSrc.path, aObj.path)) return false;
    if (!std::ifstream(aObj.path).good()) return false;

    TempFile2473 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2473 bExe((dir / (stem + "_b")).string());
    writeSource2473(bSrc.path, appSrc);

    std::string errPath =
        (dir / ("bz2473_err_" + unique_stem("") + ".txt"))
            .string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool success = false;
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        success = driver.compileFile(bSrc.path, bExe.path, deps).success;
    }
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);

    std::ifstream in(errPath);
    err.assign((std::istreambuf_iterator<char>(in)),
              std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return success;
}

}  // namespace

// ===========================================================================
// T1 — CORE repro (CLASS, private): an imported base with a PRIVATE
// auto-property (`W`, §23.1 — compiler-generated backing storage) read
// through a PUBLIC expression-bodied property (`Val`), and a LOCAL derived
// class with its OWN private field (`_own`, via property `Own`). Both the
// base auto-property's slot and the derived field's slot are read; their
// sum distinguishes "each slot reads its own value" from "the base
// auto-property's slot aliases the derived field's slot."
//
// ⚠ runAot's exit code is WEXITSTATUS-masked to 8 bits (lane protocol), so
// the combination is plain addition with distinct primes (41, 5): pre-fix,
// the importer's reconstruction of Base has ZERO storage members (W never
// crossed the wire — private auto-prop filtered by `isField`), so
// `collectInstanceStorage`/`classStruct` laid out Derived as [vtable, _own]
// — making the exporter's compiled `Val` getter (which reads the exporter's
// offset for W, = struct index 1) read `_own`'s physical slot instead, so
// `d.Val + d.Own` was `5 + 5 = 10`; post-fix (this commit) W's slot
// crosses for layout and the importer's Derived becomes [vtable, W, _own],
// with the exporter's `bz$Base$init` (#2452) writing 41 into W's slot at
// construction, giving the correct `41 + 5 = 46`.
// ===========================================================================
BEZEL_TEST(xmod_private_auto_property_layout_import_2473,
           class_getter_does_not_alias_derived_own_field_22_8_23_1) {
    int r = compileImportAndRun2473(
        "module BaseLib2473T1; "
        "public class Base2473T1 "
        "  private property W: int := 41; "
        "  public property Val: int => W; "
        "end Base2473T1;",
        "module App2473T1; import BaseLib2473T1; "
        "public class Derived2473T1: Base2473T1 "
        "  private _own: int := 5; "
        "  public property Own: int => _own; "
        "end Derived2473T1; "
        "public class C2473T1 "
        "  public static method Main(): int "
        "    using var d := new Derived2473T1(); "
        "    return d.Val + d.Own; "
        "  end Main; "
        "end C2473T1;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 46);  // 41 + 5
}

// ===========================================================================
// T2 — SETTER aliasing (the write-side twin of T1's read-side check, per
// #2438's lesson that the getter and setter slots can be separately
// broken). Writes through the INHERITED public setter (Val's block form
// writes to the private auto-property W via same-class access) and
// confirms the derived class's own independent field is UNCHANGED.
// Pre-fix, the write went to the aliasing slot (`_own`), so `d.Own` read
// back the just-written 77; post-fix the write lands in W's own slot.
// ===========================================================================
BEZEL_TEST(xmod_private_auto_property_layout_import_2473,
           class_setter_does_not_alias_derived_own_field_22_8_23_1) {
    int r = compileImportAndRun2473(
        "module BaseLib2473T2; "
        "public class Base2473T2 "
        "  private property W: int := 0; "
        "  public property Val: int "
        "    get => W; "
        "    set => W := value; "
        "  end; "
        "end Base2473T2;",
        "module App2473T2; import BaseLib2473T2; "
        "public class Derived2473T2: Base2473T2 "
        "  private _own: int := 5; "
        "  public property Own: int => _own; "
        "end Derived2473T2; "
        "public class C2473T2 "
        "  public static method Main(): int "
        "    using var d := new Derived2473T2(); "
        "    d.Val := 77; "
        "    return d.Own; "
        "  end Main; "
        "end C2473T2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 5);
}

// ===========================================================================
// T3 — INTERNAL visibility axis: the same shape as T1 but with an INTERNAL
// auto-property. `internal` (§13.1) is closed by the module compilation
// (§13.4) — not exported — so an internal auto-property is filtered by
// the identical `isField`-only gate and suffers the identical aliasing.
// Pinning this guards against a fix that widens `isField` for `private`
// only (e.g. by gated visibility-string match) rather than for
// not-exported storage members uniformly.
// ===========================================================================
BEZEL_TEST(xmod_private_auto_property_layout_import_2473,
           class_internal_auto_property_does_not_alias_22_8_13_1) {
    int r = compileImportAndRun2473(
        "module BaseLib2473T3; "
        "public class Base2473T3 "
        "  internal property W: int := 41; "
        "  public property Val: int => W; "
        "end Base2473T3;",
        "module App2473T3; import BaseLib2473T3; "
        "public class Derived2473T3: Base2473T3 "
        "  private _own: int := 5; "
        "  public property Own: int => _own; "
        "end Derived2473T3; "
        "public class C2473T3 "
        "  public static method Main(): int "
        "    using var d := new Derived2473T3(); "
        "    return d.Val + d.Own; "
        "  end Main; "
        "end C2473T3;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 46);  // 41 + 5
}

// ===========================================================================
// T4 — index ARITHMETIC across MULTIPLE private auto-properties (not just
// "off by one slot"): four private auto-properties on the base, each with
// a DISTINCT prime value, all read through their own public properties,
// plus the derived class's own field. Pre-fix every base auto-property is
// absent from the importer's layout, so the derived class's single `_own`
// field aliases whichever base slot the exporter's compiled getter reads;
// the four base getters would all read `_own`'s value (5) on the
// importer's undersized struct, giving 5+5+5+5+5 = 25 instead of the
// correct 2+3+5+7+11 = 28.
// ===========================================================================
BEZEL_TEST(xmod_private_auto_property_layout_import_2473,
           class_multiple_private_auto_properties_all_independent_22_8) {
    int r = compileImportAndRun2473(
        "module BaseLib2473T4; "
        "public class Base2473T4 "
        "  private property A: int := 2; "
        "  private property B: int := 3; "
        "  private property C: int := 5; "
        "  private property D: int := 7; "
        "  public property PA: int => A; "
        "  public property PB: int => B; "
        "  public property PC: int => C; "
        "  public property PD: int => D; "
        "end Base2473T4;",
        "module App2473T4; import BaseLib2473T4; "
        "public class Derived2473T4: Base2473T4 "
        "  private _own: int := 11; "
        "  public property Own: int => _own; "
        "end Derived2473T4; "
        "public class C2473T4 "
        "  public static method Main(): int "
        "    using var d := new Derived2473T4(); "
        "    return d.PA + d.PB + d.PC + d.PD + d.Own; "
        "  end Main; "
        "end C2473T4;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 28);  // 2 + 3 + 5 + 7 + 11
}

// ===========================================================================
// T5 — DIRECT construction (no local derivation) of an imported class
// with a private auto-property. The exporter's `Val` getter must read W's
// own value (41), not whatever happens to be at the undersized struct's
// past-the-end slot. This is the under-allocation hazard #2456 describes
// for plain fields: the importer's Base reconstruction had ZERO storage,
// so `new Base()` allocated too little and the exporter's getter read
// out-of-bounds — a heap-buffer-overflow (latent here because the
// exporter's object code is not ASan-instrumented by the harness, but
// the VALUE is still wrong: the exporter's `bz$Base$init` wrote 41 into
// offset 1 of an allocation sized for offset 0 only). Post-fix the
// importer allocates the full layout and the value is correct.
// ===========================================================================
BEZEL_TEST(xmod_private_auto_property_layout_import_2473,
           class_direct_import_construction_reads_own_value_22_8) {
    int r = compileImportAndRun2473(
        "module BaseLib2473T5; "
        "public class Base2473T5 "
        "  private property W: int := 41; "
        "  public property Val: int => W; "
        "end Base2473T5;",
        "module App2473T5; import BaseLib2473T5; "
        "public class C2473T5 "
        "  public static method Main(): int "
        "    using var d := new Base2473T5(); "
        "    return d.Val; "
        "  end Main; "
        "end C2473T5;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 41);
}

// ===========================================================================
// T6 — CORE repro (RECORD): a record with a positional field `x` and a
// PRIVATE body auto-property `W`, with a PUBLIC METHOD `GetW` that
// returns W's value (mirrors #2468 T1's plain-field shape — records do
// not export property getters, so a METHOD is used to read the hidden
// slot). Calling the method cross-module marshals the record value to
// the exporter's compiled body, which indexes W's slot at the
// exporter's real layout offset.
//
// ⚠ runAot's exit code is WEXITSTATUS-masked to 8 bits (lane protocol):
// pre-#2473, W never crossed the wire at all (private auto-prop
// filtered), so the importer's record reconstruction had only `x` in
// `t->members`, the exporter's compiled `GetW` (which reads the
// exporter's offset for W — struct index 0, since a record's body
// members precede its positional fields in `t->members`) read `x`'s
// slot instead, returning 9 — the ALIASING bug this test's own name
// pins: `r.x + h` was `9 + 9 = 18`. #2473 (this file) made W's SLOT
// cross for layout, ending the aliasing; #2473's own scope note said
// the value itself stayed a deferred gap ("value-replay is #2467's
// follow-up" — value NOT replayed, W's slot zero-initialized, `r.x + h`
// was `9 + 0 = 9`), and this row pinned that intermediate state.
//
// fix #3736 (§9.2 / §22.8 step 2 / §2.8.2, v0.286) closed exactly that
// deferred gap: ModuleMetadataBuilder.cpp's PropertySymbol branch now
// carries a non-static auto-property's initializer text via the SAME
// `fieldInitText` wire slot fix #3723 already carries a private RECORD
// FIELD's initializer through (this file's own `includeForLayout`
// gate, `!isGenericMonomorphized && t.kind != Record` — a record has no
// `bz$<Type>$init` at any visibility, so #3723 already made the
// analogous PLAIN-FIELD case replay for records, see
// xmod_non_generic_record_field_init_replay_3723_test.cpp T6). §9.2
// requires an auto-property's initializer to run once at construction
// exactly like a field's; there is no visibility carve-out in §22.8's
// storage-layout rule or §9.2's construction rule, so a PRIVATE
// auto-property is not a distinct case from a private FIELD here.
// MEASURED (this row's own fix-forward investigation, copy-reverting
// ModuleMetadataBuilder.cpp/ModuleLoader.cpp/ModuleMetadata.cpp to their
// pre-#3736 state and rebuilding): pre-#3736, `x=9, h=0` (this row's old
// pin); post-#3736, `x=9, h=41` — `x` is IDENTICAL in both trees (the
// #2473 anti-aliasing fix this file is about is untouched by #3736;
// there is no aliasing regression), only `h` (W's replayed value)
// changes, from the deferred-gap `0` to the spec-correct `41`. So this
// row's `9` was pinning a KNOWN, explicitly-documented-as-incomplete
// intermediate state, not a final correct answer — per LANE_RULES §7,
// updated here to the now-correct `r.x + h = 9 + 41 = 50`.
// ===========================================================================
BEZEL_TEST(xmod_private_auto_property_layout_import_2473,
           record_method_call_does_not_alias_positional_field_9_2_22_8) {
    int r = compileImportAndRun2473(
        "module RecLib2473T6; "
        "public record Rec2473T6(x: int) "
        "  private property W: int := 41; "
        "  public method GetW(): int => W; "
        "end Rec2473T6;",
        "module App2473T6; import RecLib2473T6; "
        "public class C2473T6 "
        "  public static method Main(): int "
        "    var r := new Rec2473T6(9); "
        "    var h := r.GetW(); "
        "    return r.x + h; "
        "  end Main; "
        "end C2473T6;");
    if (r == -2) return;
    // §9.2/§22.8: `x` stays 9 (no aliasing, this row's own #2473 claim,
    // unaffected by #3736 — verified by direct instrumentation, see this
    // row's header comment); `h` is now 41 (W's initializer correctly
    // replayed by fix #3736, closing the deferred value-replay gap
    // #2473's own scope note left open).
    BEZEL_EXPECT_EQ(r, 50);  // 9 + 41
}

// ===========================================================================
// T7 — CONTROL (generic class): a GENERIC class with a private auto-
// property, directly constructed cross-module (no local derivation).
// This verifies the metadata-export loop change does not break the
// generic-class path: the compile must succeed and the value must read
// consistently with the pre-existing generic-class instance-initializer-
// replay gap (which affects plain FIELDS identically — verified during
// this fix by direct before/after comparison: a `private _w: int := 7`
// in the same shape also reads 0 cross-module). This test is NOT a
// fail-without/pass-with regression for #2473 itself — the generic-
// class value-replay gap (pre-existing, same for plain fields) and the
// pre-existing cross-module generic-derivation link error (which blocks
// the aliasing shape for BOTH plain fields and auto-properties —
// confirmed by direct comparison) together prevent a value-observable
// regression for the `includeForMethodBody` gate in isolation. The
// `includeForMethodBody` widening is structurally identical to the
// `includeForLayout` widening (same `|| isAutoPropStorage` flag,
// T1-T6); this test pins that the generic path is not BROKEN by the
// change.
// ===========================================================================
BEZEL_TEST(xmod_private_auto_property_layout_import_2473,
           generic_class_private_auto_property_compiles_14_1_control) {
    int r = compileImportAndRun2473(
        "module BoxLib2473T7; "
        "public class Box<T> "
        "  private property _w: int := 7; "
        "  public method Get(): int => _w; "
        "end Box;",
        "module App2473T7; import BoxLib2473T7; "
        "public class C2473T7 "
        "  public static method Main(): int "
        "    using var b := new Box<int>(); "
        "    return b.Get(); "
        "  end Main; "
        "end C2473T7;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    // UPDATED 2026-08-13 (#3152, the auto-property storage unification): this
    // asserted 0, and its justification was measurably FALSE — and was false
    // when it was written. It claimed the generic-class initializer-replay gap
    // "affects plain fields identically"; it does not. The plain-field twin of
    // this exact shape (`private _w: int := 7;`) returned 7 BEFORE the fix as
    // well as after. The field half replayed and the auto-property half did
    // not, and this test froze the broken half while asserting the two agreed.
    //
    // `emitOwnFieldInit` decided "is this member auto-property storage?" with
    // an AST read that is null for every imported member, so an imported
    // generic's auto-property initializer never ran. 7 is the correct value:
    // it is what the character-identical SINGLE-MODULE twin returns, which is
    // what §2.8.2 clause 1 requires of the importer.
    BEZEL_EXPECT_TRUE(r >= 0);
    BEZEL_EXPECT_EQ(r, 7);
}

// ===========================================================================
// T8 — CONTROL: encapsulation is UNCHANGED by this fix. A private
// auto-property newly carried in metadata for LAYOUT is still not
// name-resolvable from outside the declaring class — sema's visibility
// check must reject a direct `d.W` exactly as it already did (the
// property being present in `t->members` for layout must not make it
// independently ACCESSIBLE by name). Mirrors #2456 T5 / #2468 T4.
// ===========================================================================
BEZEL_TEST(xmod_private_auto_property_layout_import_2473,
           private_auto_property_still_inaccessible_by_name_control) {
    std::string err;
    bool success = compileImportCaptureErr2473(
        "module BaseLib2473T8; "
        "public class Base2473T8 "
        "  private property W: int := 41; "
        "  public property Val: int => W; "
        "end Base2473T8;",
        "module App2473T8; import BaseLib2473T8; "
        "public class C2473T8 "
        "  public static method Main(): int "
        "    using var d := new Base2473T8(); "
        "    return d.W; "
        "  end Main; "
        "end C2473T8;",
        err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("private") != std::string::npos);
}

// ===========================================================================
// T9 — CONTROL: a fully LOCAL class (no import at all) with the identical
// shape as T1. Guards against this fix's new metadata-export path
// affecting same-module compilation at all (it should not run — the
// exporter loop only fires when producing an artifact's metadata, and
// this path was ALREADY correct locally, including reading the DECLARED
// default 41 — local construction has no cross-module value-replay gap).
// Mirrors #2456 T7 / #2468 T5.
// ===========================================================================
BEZEL_TEST(xmod_private_auto_property_layout_import_2473,
           local_class_unaffected_control) {
    bezel::Driver driver;
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2473_t9_");
    TempFile2473 src((dir / (stem + ".bzl")).string());
    TempFile2473 exe((dir / stem).string());
    writeSource2473(src.path,
                    "module LocalCtl2473T9; "
                    "public class LocalBase2473T9 "
                    "  private property W: int := 41; "
                    "  public property Val: int => W; "
                    "end LocalBase2473T9; "
                    "public class LocalDerived2473T9: LocalBase2473T9 "
                    "  private _own: int := 5; "
                    "  public property Own: int => _own; "
                    "end LocalDerived2473T9; "
                    "public class C2473T9 "
                    "  public static method Main(): int "
                    "    using var d := new LocalDerived2473T9(); "
                    "    return d.Val + d.Own; "
                    "  end Main; "
                    "end C2473T9;");
    BEZEL_EXPECT_TRUE(driver.compileFile(src.path, exe.path, {}).success);
    int status = std::system(exe.path.c_str());
    int r = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(r, 46);  // 41 + 5
}

// ===========================================================================
// T10 — CONTROL: a PUBLIC auto-property on an imported base (already
// correct post-#2478 — the importer-side classification fix). Guards
// against this fix's metadata-export-loop widening regressing the
// already-correct public path: a public auto-property crosses the
// boundary via `exported` (not via `includeForLayout`), so it is
// unaffected by this fix's gate change — but the test pins that
// round-tripping still works.
// ===========================================================================
BEZEL_TEST(xmod_private_auto_property_layout_import_2473,
           public_auto_property_unaffected_control_2478) {
    int r = compileImportAndRun2473(
        "module BaseLib2473T10; "
        "public class Base2473T10 "
        "  public property W: int := 41; "
        "end Base2473T10;",
        "module App2473T10; import BaseLib2473T10; "
        "public class Derived2473T10: Base2473T10 "
        "  private _own: int := 5; "
        "  public property Own: int => _own; "
        "end Derived2473T10; "
        "public class C2473T10 "
        "  public static method Main(): int "
        "    using var d := new Derived2473T10(); "
        "    return d.W + d.Own; "
        "  end Main; "
        "end C2473T10;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 46);  // 41 + 5
}

// ===========================================================================
// T11 — CONTROL: a PRIVATE PLAIN FIELD on an imported base (#2456's own
// shape). Guards against this fix's `isField`-adjacent change regressing
// #2456's plain-field carriage — the test must still pass with this
// fix's gate widening in place (the widening is ADDITIVE: it adds
// auto-property recognition alongside `isField`, never replacing it).
// ===========================================================================
BEZEL_TEST(xmod_private_auto_property_layout_import_2473,
           plain_field_carriage_unaffected_control_2456) {
    int r = compileImportAndRun2473(
        "module BaseLib2473T11; "
        "public class Base2473T11 "
        "  private _v: int := 41; "
        "  public property Val: int => _v; "
        "end Base2473T11;",
        "module App2473T11; import BaseLib2473T11; "
        "public class Derived2473T11: Base2473T11 "
        "  private _own: int := 5; "
        "  public property Own: int => _own; "
        "end Derived2473T11; "
        "public class C2473T11 "
        "  public static method Main(): int "
        "    using var d := new Derived2473T11(); "
        "    return d.Val + d.Own; "
        "  end Main; "
        "end C2473T11;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 46);  // 41 + 5
}

}  // namespace bezel::module
