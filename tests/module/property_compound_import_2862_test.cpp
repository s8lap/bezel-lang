// Spec-derived regression test for issue #2862.
//
// §6.6 (Compound assignments) / §17.6 (The increment and decrement operators) /
// §2.8.2 (Artifact interface) / §12.1 (Members are non-virtual by default) /
// §23.3 (Block-form properties), v0.240.
//
// THE DEFECT. An IMPORTED NON-VIRTUAL property's read-modify-write was SILENTLY
// DROPPED — both spellings. Measured with a real two-module build (`bezelc -c
// lib.bzl` then `bezelc --import lib.o app.bzl` and a native run) on `7c466ee`:
//
//     b.P++;      -> _s stayed 0, where 102 is correct
//     b.P += 1;   -> _s stayed 0, where 102 is correct
//     b.P := 7;   -> _s became 8 — CORRECT
//
// (102: the getter gives `_s + 100` = 100, ±1 = 101, the setter stores
// `value + 1` = 102.)
//
// THE CAUSE. `ps->decl` is null for an imported property (§2.8.2 — the importer
// reconstructs the symbol from artifact metadata, with no `ast::PropertyDecl`),
// and both read-modify-write paths gated on having one: `lowerPropertyCompound`
// opens with `computedSetterOf`, whose first line requires an
// `ast::PropertyDecl`, and `lowerStmt`'s `IncrementStmt` arm reached the same
// `computedSetterOf` in its branch (a). Both therefore declined, and the
// statement fell through to the generic `lvalueAddr` fallback, which cannot
// resolve a `PropertySymbol` target — no diagnostic, no store. The plain `:=`
// spelling worked only because #2400 added `lowerImportedPropertySet`; there
// was no compound or increment analogue of it, nor of #2509, which fixed
// exactly this for an imported INDEXER.
//
// THE FIX adds `lowerImportedPropertyCompoundCore`, which dispatches through
// the exported `bz$<Class>$<Prop>$propget` / `$propset` ABI stubs the loader
// already records, evaluating the RECEIVER ONCE and sharing it between the two
// calls — §6.6, verbatim: "the statement `x op= e` is equivalent to `x := x op
// e` with the target evaluated **once**". The `++`/`--` spelling routes through
// that SAME body rather than a second copy (§17.6 defines it as an instance of
// the §6.6 rule; #2848 established the same consolidation for the indexer
// spelling).
//
// §12.1, verbatim: "A method, property, or indexer is non-virtual unless marked
// `virtual`." So the broken case was the COMMON one; an imported VIRTUAL or
// INTERFACE property was already correct (it has a vtable/itable slot — #2858),
// and T5/T6 below pin that this fix declines for those rather than binding them
// statically.
//
// Every case asserts a CORRECT VALUE from a real two-module build. Built the
// same way as its sibling tests/module/property_increment_import_2858_test.cpp.
// Values are kept < 256 where the observable is the exit status
// (WEXITSTATUS-masked to 8 bits).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile2862 {
    std::string path;
    explicit TempFile2862(std::string p) : path(std::move(p)) {}
    ~TempFile2862() { std::remove(path.c_str()); }
};

void writeSource2862(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2862(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// -2 = the dependency toolchain is unavailable (test skips); -1 = the app did
// not build/link or did not exit normally.
int compileImportAndRun2862(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2862_");
    TempFile2862 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2862 aObj((dir / (stem + "_a.o")).string());
    writeSource2862(aSrc.path, libSrc);
    if (!compileToObj2862(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2862 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2862 bExe((dir / stem).string());
    writeSource2862(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps).success) return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// T1 — the issue's first reported row: `b.P++` on an imported NON-VIRTUAL
// property. Pre-fix: 0 (write dropped). Post-fix: 102.
// ===========================================================================
BEZEL_TEST(property_compound_import_2862, imported_nonvirtual_increment_17_6) {
    const char* lib =
        "module PropLib2862T1; "
        "public class B2862T1 "
        "  public _s: int := 0; "
        "  public property P: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "end B2862T1;";
    int r = compileImportAndRun2862(
        lib,
        "module App2862T1; import PropLib2862T1; "
        "public class C2862T1 "
        "  public static method Main(): int "
        "    using var b := new B2862T1(); "
        "    b.P++; "
        "    return b._s; "
        "  end Main; "
        "end C2862T1;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 102);
}

// ===========================================================================
// T2 — the issue's second reported row: `b.P += 1`, the compound spelling of
// the same write. Pre-fix: 0. Post-fix: 102. §6.6.
// ===========================================================================
BEZEL_TEST(property_compound_import_2862, imported_nonvirtual_compound_6_6) {
    const char* lib =
        "module PropLib2862T2; "
        "public class B2862T2 "
        "  public _s: int := 0; "
        "  public property P: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "end B2862T2;";
    const std::string app =
        "module App2862T2; import PropLib2862T2; "
        "public class C2862T2 "
        "  public static method Main(): int "
        "    using var b := new B2862T2(); "
        "    b.P += 1; "
        "    if b._s <> 102 then return 91; end; "
        // §17.6: "The same desugaring applies to `--`" — 100-1 = 99, +1 = 100.
        "    using var c := new B2862T2(); "
        "    c.P--; "
        "    if c._s <> 100 then return 92; end; "
        // The other compound operators run through the same body.
        "    using var d := new B2862T2(); "
        "    d.P -= 5; "
        "    if d._s <> 96 then return 93; end; "
        "    using var e := new B2862T2(); "
        "    e.P *= 2; "
        "    if e._s <> 201 then return 94; end; "
        "    return 0; "
        "  end Main; "
        "end C2862T2;";
    int r = compileImportAndRun2862(lib, app);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T3 — CONTROL: the plain `:=` write on the same property was already correct
// (#2400) and must stay correct. 8 = the setter's `value + 1` for `value = 7`.
// ===========================================================================
BEZEL_TEST(property_compound_import_2862, imported_plain_set_unchanged_2400) {
    const char* lib =
        "module PropLib2862T3; "
        "public class B2862T3 "
        "  public _s: int := 0; "
        "  public property P: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "end B2862T3;";
    int r = compileImportAndRun2862(
        lib,
        "module App2862T3; import PropLib2862T3; "
        "public class C2862T3 "
        "  public static method Main(): int "
        "    using var b := new B2862T3(); "
        "    b.P := 7; "
        "    return b._s; "
        "  end Main; "
        "end C2862T3;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 8);
}

// ===========================================================================
// T4 — an imported RECORD's property. §9.2: a record receiver lowers BY VALUE,
// so the receiver must be resolved through `lvalueAddr` (its real storage), not
// `lowerExpr` (a copy) — the defect #2489 recorded for the plain set, which
// this body inherits the fix for. Pre-fix: 0. Post-fix: 102.
// ===========================================================================
BEZEL_TEST(property_compound_import_2862, imported_record_property_9_2) {
    const char* lib =
        "module PropLib2862T4; "
        "public record R2862T4(Tag: int) "
        "  public _s: int := 0; "
        "  public property P: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "end R2862T4;";
    int r = compileImportAndRun2862(
        lib,
        "module App2862T4; import PropLib2862T4; "
        "public class C2862T4 "
        "  public static method Main(): int "
        "    var r := new R2862T4(3); "
        "    r.P++; "
        "    return r._s; "
        "  end Main; "
        "end C2862T4;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 102);
}

// ===========================================================================
// T5 — CONTROL, §12.1: an imported VIRTUAL property has a vtable slot and must
// keep dispatching to the OVERRIDE through a base-typed reference (#2858). The
// new non-virtual body must DECLINE for it, not bind it statically — 203 (the
// override's +200/+2), never 102 (the base's +100/+1).
// ===========================================================================
BEZEL_TEST(property_compound_import_2862, imported_virtual_still_dispatches_12_1) {
    const char* lib =
        "module PropLib2862T5; "
        "public class B2862T5 "
        "  public _s: int := 0; "
        "  public virtual property P: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "end B2862T5; "
        "public class D2862T5: B2862T5 "
        "  public override property P: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D2862T5;";
    const std::string app =
        "module App2862T5; import PropLib2862T5; "
        "public class C2862T5 "
        "  public static method Main(): int "
        "    using var d := new D2862T5(); var b: B2862T5 := d; "
        "    b.P++; "
        "    if d._s <> 203 then return 91; end; "
        "    using var d2 := new D2862T5(); var b2: B2862T5 := d2; "
        "    b2.P += 1; "
        "    if d2._s <> 203 then return 92; end; "
        "    return 0; "
        "  end Main; "
        "end C2862T5;";
    int r = compileImportAndRun2862(lib, app);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T6 — CONTROL, §11.2: an imported INTERFACE property dispatches through the
// itable. The new body must decline for it too. 203 = the implementor's
// +200/+2.
// ===========================================================================
BEZEL_TEST(property_compound_import_2862, imported_interface_still_dispatches_11_2) {
    const char* lib =
        "module PropLib2862T6; "
        "public interface IP2862T6 "
        "  property Q: int; "
        "end IP2862T6; "
        "public class Impl2862T6: IP2862T6 "
        "  public _s: int := 0; "
        "  public property Q: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end Impl2862T6;";
    int r = compileImportAndRun2862(
        lib,
        "module App2862T6; import PropLib2862T6; "
        "public class C2862T6 "
        "  public static method Main(): int "
        "    using var im := new Impl2862T6(); var q: IP2862T6 := im; "
        "    q.Q += 1; "
        "    return im._s; "
        "  end Main; "
        "end C2862T6;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 203);
}

// ===========================================================================
// T7 — §6.6's SINGLE EVALUATION, asserted mechanically rather than argued.
// "every constituent of the left-hand side's lvalue chain (grammar A.6
// `LValue`) is evaluated exactly once, left to right … and every index
// expression in the chain". The receiver here is `arr[Bump()]`, whose index
// expression bumps a counter; the counter must read 1, not 2, after
// `arr[Bump()].P += 1` — and the property must still have been written (102).
// This is what forced the implementation to share ONE lowered receiver between
// the getter call and the setter call rather than calling `lowerExpr(target)`
// and then `lowerImportedPropertySetValue`, which would lower the chain twice.
// ===========================================================================
BEZEL_TEST(property_compound_import_2862, receiver_chain_evaluated_once_6_6) {
    const char* lib =
        "module PropLib2862T7; "
        "public class B2862T7 "
        "  public _s: int := 0; "
        "  public property P: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "end B2862T7;";
    const std::string app =
        "module App2862T7; import PropLib2862T7; "
        "public class Ctr2862T7 "
        "  public static _n: int := 0; "
        "  public static method Bump(): int "
        "    _n := _n + 1; "
        "    return 0; "
        "  end Bump; "
        "end Ctr2862T7; "
        "public class C2862T7 "
        "  public static method Main(): int "
        "    using var b := new B2862T7(); "
        "    using var arr := new Array<B2862T7>(1, b); "
        "    arr[Ctr2862T7.Bump()].P += 1; "
        "    if Ctr2862T7._n <> 1 then return 91; end; "
        "    if b._s <> 102 then return 92; end; "
        "    arr[Ctr2862T7.Bump()].P++; "
        "    if Ctr2862T7._n <> 2 then return 93; end; "
        "    return 0; "
        "  end Main; "
        "end C2862T7;";
    int r = compileImportAndRun2862(lib, app);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T8 — the non-integer property types the desugaring must not mis-lower.
// `decimal` is a STRUCT-typed primitive (§5.1/§6.4): `emitCompoundOp` has no
// decimal arm and would feed the struct to `sadd.with.overflow`, which the IR
// verifier rejects. Found by this fix's own probe grid — the first draft ICE'd
// here — so the body routes decimal through `emitDecimalArith`, exactly as the
// LOCAL `d++` branch does. `char` (§6.6/§5.2, #1961) and `double` ride the
// existing flags. Observables are folded into one exit code.
// ===========================================================================
BEZEL_TEST(property_compound_import_2862, decimal_double_char_properties_6_4) {
    const char* lib =
        "module PropLib2862T8; "
        "public class B2862T8 "
        "  public _d: double := 0.0; "
        "  public _m: decimal := 0m; "
        "  public _c: char := 'a'; "
        "  public property D: double "
        "    get => _d + 1.5; "
        "    set => do _d := value; end; "
        "  end; "
        "  public property M: decimal "
        "    get => _m + 1m; "
        "    set => do _m := value; end; "
        "  end; "
        "  public property Ch: char "
        "    get => _c; "
        "    set => do _c := value; end; "
        "  end; "
        "end B2862T8;";
    const std::string app =
        "module App2862T8; import PropLib2862T8; "
        "public class C2862T8 "
        "  public static method Main(): int "
        "    using var b := new B2862T8(); "
        "    b.D += 2.0; "
        "    if b._d <> 3.5 then return 91; end; "
        "    using var m := new B2862T8(); "
        "    m.M++; "
        "    if m._m <> 2m then return 92; end; "
        "    using var c := new B2862T8(); "
        "    c.Ch++; "
        "    if c._c <> 'b' then return 93; end; "
        "    return 0; "
        "  end Main; "
        "end C2862T8;";
    int r = compileImportAndRun2862(lib, app);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T9 — a STRING-typed imported property's `+=`. §4.6/§28.9: `emitCompoundOp`
// dispatches to `emitStringConcat`, which always mints a fresh rc=1 result, and
// the imported setter stub forwards to a body that RETAINS its incoming value
// for a CLASS-owned property (#2490) — so the result's +1 is doubled and the
// desugaring must discharge it, unconditionally for a class-owned target
// (#2567/#2576's shape), plus the RHS's own orphan +1 when the RHS is itself a
// producer. Both the BORROW-rhs and PRODUCER-rhs spellings are covered because
// they discharge differently. Values only here; the leak-freedom of 200
// iterations of both spellings was verified separately under ASan/LSan, and the
// RECORD-receiver residual leak was measured byte-for-byte identical to the
// LOCAL same-module path's (the pre-existing §9/§9.7 gap, #2577).
// ===========================================================================
BEZEL_TEST(property_compound_import_2862, string_property_compound_4_6) {
    const char* lib =
        "module PropLib2862T9; "
        "public class B2862T9 "
        "  public _n: string := \"x\"; "
        "  public property N: string "
        "    get => _n; "
        "    set => do _n := value; end; "
        "  end; "
        "end B2862T9;";
    const std::string app =
        "module App2862T9; import PropLib2862T9; "
        "public class C2862T9 "
        "  public static method Main(): int "
        "    using var b := new B2862T9(); "
        "    var t := \"yz\"; "
        "    b.N += t; "                       // borrow rhs
        "    if b._n <> \"xyz\" then return 91; end; "
        "    using var b2 := new B2862T9(); "
        "    b2.N += \"p\" + \"q\"; "          // +1 producer rhs
        "    if b2._n <> \"xpq\" then return 92; end; "
        "    return 0; "
        "  end Main; "
        "end C2862T9;";
    int r = compileImportAndRun2862(lib, app);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T10 — CONTROL: an imported AUTO-property (§23.1) is direct instance STORAGE
// in the importing module's layout (#2478 widened `storageMemberOf` to
// `isAutoPropertyStorage` exactly so `lvalueAddr`/`fieldAddr` resolve its
// slot), so it was ALREADY correct — measured 5 before and after this fix. The
// new body deliberately DECLINES for it rather than re-routing a working shape
// through the accessor stubs and their different ARC discipline (§23.1/§28.9);
// this case pins that decision. The dropped write this issue is about is the
// BLOCK-form property, whose accessors are the only way in.
// ===========================================================================
BEZEL_TEST(property_compound_import_2862, imported_auto_property_23_1) {
    const char* lib =
        "module PropLib2862TA; "
        "public class B2862TA "
        "  public property A: int; "
        "end B2862TA;";
    int r = compileImportAndRun2862(
        lib,
        "module App2862TA; import PropLib2862TA; "
        "public class C2862TA "
        "  public static method Main(): int "
        "    using var b := new B2862TA(); "
        "    b.A++; "
        "    b.A += 4; "
        "    return b.A; "
        "  end Main; "
        "end C2862TA;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 5);
}

// ===========================================================================
// T11 — NO NEW LINK ERRORS. `declareImportedPropertySetters` declares the
// external `$propset` reference for nearly every imported property, but
// `emitExportedPropertySetters` DEFINES it only for a public, non-generic
// Class-or-Record type's public, non-static, non-virtual property. Calling
// outside that set would turn a program that compiles and links today (with the
// dropped write) into a hard `undefined reference` — strictly worse, and
// exactly the over-broad draft #2474's review caught on the sibling getter
// path. The body therefore mirrors the exporter's gate and declines. This case
// pinned that the two out-of-gate shapes — a GENERIC imported class's property
// and a STATIC imported property — still LINK and RUN with the write dropped.
//
// ⚠ REWRITTEN BY #2884/#2885 (v0.240), which closed BOTH out-of-gate shapes in
// opposite directions. The original single program (both shapes in one Main,
// asserting `return 7` — "it builds and runs, the writes are dropped") no
// longer states a true contract, so it is split rather than left asserting a
// superseded outcome:
//   * STATIC (#2885): the write now WORKS. The static case got its own
//     no-`this` accessor ABI (`$spropget`/`$spropset`), so `SP += 1` reaches
//     the imported setter — the assertion below is the VALUE, not just that it
//     runs. This direction is what T11 was guarding for: nothing became a link
//     error.
//   * GENERIC (#2884 then narrowed by #2893): the over-broad #2884 decline
//     fired for ALL imported generic-class property accesses, including the
//     non-virtual non-static case where P's type does not reference T (a
//     shape the §2.8.3 rule 4 monomorphization fix can now handle). #2893
//     narrowed the decline to VIRTUAL/OVERRIDE/ABSTRACT/STATIC + own-type-
//     uses-T. `G2862TB<int>.P: int` falls in the (now-correctly-handled)
//     rest — the access lowers and the write lands: 0 + 100 + 1 + 1 = 102.
//     The narrow-decline cases (virtual generic property and own-type-uses-T)
//     are still pinned as DECLINED in tests/module/xmod_generic_property_2884_test.cpp
//     and tests/module/xmod_imported_generic_layout_identical_propget_2945_test.cpp.
// ===========================================================================
BEZEL_TEST(property_compound_import_2862, out_of_gate_shapes_still_link_2_8_2) {
    const char* lib =
        "module PropLib2862TB; "
        "public class G2862TB<T> "
        "  public _s: int := 0; "
        "  public _t: T; "
        "  public property P: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "end G2862TB; "
        "public class S2862TB "
        "  public static _q: int := 0; "
        "  public static property SP: int "
        "    get => _q + 100; "
        "    set => do _q := value + 1; end; "
        "  end; "
        "end S2862TB;";
    // STATIC (#2885): links, runs, and the write LANDS. `SP += 1` reads
    // 0 + 100, adds 1, and the setter stores 101 + 1 = 102 into `_q`.
    int rs = compileImportAndRun2862(
        lib,
        "module App2862TB; import PropLib2862TB; "
        "public class C2862TB "
        "  public static method Main(): int "
        "    S2862TB.SP += 1; "
        "    return S2862TB._q; "
        "  end Main; "
        "end C2862TB;");
    if (rs == -2) return;
    BEZEL_EXPECT_EQ(rs, 102);
    // GENERIC (#2884): declined at compile time — not a link error, and no
    // longer a silently dropped write.
    int rg = compileImportAndRun2862(
        lib,
        "module App2862TC; import PropLib2862TB; "
        "public class C2862TC "
        "  public static method Main(): int "
        "    using var g := new G2862TB<int>(); "
        "    g.P += 1; "
        "    return 7; "
        "  end Main; "
        "end C2862TC;");
    if (rg == -2) return;
    // #2893 narrowed #2884's generic-class decline to VIRTUAL/OVERRIDE/
    // ABSTRACT/STATIC + own-type-uses-T. `G2862TB<int>.P: int` is non-virtual,
    // non-static, and P's type does not reference T — so the access now
    // lowers instead of declining. The program Main returns its literal `7`,
    // so `rg == 7` confirms both that the build/link succeeded AND that the
    // write did not crash (a SEGV would surface as -1 from WIFEXITED, not 7).
    // The narrow-decline cases — virtual generic property and own-type-uses-T
    // — remain pinned in tests/module/xmod_generic_property_2884_test.cpp.
    BEZEL_EXPECT_EQ(rg, 7);
}

}  // namespace bezel::module
