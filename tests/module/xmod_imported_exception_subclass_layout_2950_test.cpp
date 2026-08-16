// Spec-derived regression test for issue #2950.
//
// §12 (Inheritance) / §31.4 (Standard exception types) / §2.5 (Modules and
// imports) / §2.8.2 (Artifact interface) / §2.8.3 rule 4 / §11.3, v0.240.
//
// THE DEFECT. A class deriving from a BUILT-IN exception carries the §31.4
// header in its instance layout — [0]=vtable, [8]=Message, [16]=InnerException,
// [24]=CauseChain, [32]=ParamName, then the disposed flag and the class's own
// fields. Whether a class carries that header was decided by walking
// `ast::TypeDecl::baseTypes`, and an IMPORTED class has no AST at all: the walk
// found nothing, silently, for every imported exception subclass. So the two
// artifacts laid the SAME class out differently. Measured on the fix's parent
// (`f069645`) with a real two-module build, for one `public class MyEx:
// Exception` carrying a single `int` field:
//
//     exporter   %class.MyEx = type { ptr, ptr, ptr, ptr, ptr, i8, i32 }   48 B
//     importer   %class.MyEx = type { ptr, i8, i32 }                       16 B
//
// The importer emits `calloc(1, 16)` and then calls the EXPORTER's compiled
// `bz$XLib.MyEx$ctor`, which stores the message at offset 8 (over the
// importer's disposed flag and its first field) and the field at offset 44 —
// 28 bytes past the end of the block. The program still "ran": that is the
// point. What it printed was wrong, and the wrongness is what these cells pin,
// because "exit 0" was never an oracle for a layout defect.
//
//     e.Val      read by the IMPORTER at its own offset 12   ->  0   (want 7)
//     e.ReadVal() read by the EXPORTER at offset 44          ->  1673780053
//
// The second value is heap junk from past the end of the block, so the issue's
// own "happens to return 7" is not even stable across runs.
//
// The same `b->decl` gate on the ancestor step stopped the walk at the module
// boundary in the OTHER direction too — T2 pins that arm: a LOCAL class whose
// base is an IMPORTED exception subclass lost the header as well
// (`%class.Local = type { ptr, i8, i32, i32 }`, and its inherited field read 0).
//
// THE FIX carries the built-in exception base name across the boundary. It
// cannot ride `baseTypes`, which is built from `sema::TypeSymbol::baseTypeRefs`
// and never held a built-in base on the EXPORTER side either (a built-in
// exception has no user TypeSymbol, so `TypeResolution` never pushes one) —
// this is the "correct predicate fed by a source collection that never
// contained the population" shape, and reading the predicate alone tells you
// nothing is wrong. So `sema::TypeSymbol` gains `builtinExceptionBase`,
// populated by Type Resolution locally and by ModuleLoader from a new
// REC_EXC_BASE metadata record, and every consumer asks the one walk
// `sema::builtinExceptionBaseOf` for it.
//
// §2.8.2 states, verbatim: "An implementation unable to carry some body shape
// in its interface format must reject the export, or the import, with a
// diagnostic naming the shape; it must not alter the instantiation's
// semantics." The pre-fix behaviour altered the instantiation's LAYOUT with no
// diagnostic at any severity. §2.8.3 rule 4 states, verbatim: "one runtime type
// identity — `is` tests, `catch` clauses, and §19 patterns over values of the
// instantiation match identically regardless of which artifact produced the
// value or performs the test".
//
// Built like its siblings tests/module/xmod_object_tostring_producer_residual_
// 2867_test.cpp and property_compound_import_2862_test.cpp: a real
// `bezelc -c lib.bzl` + `bezelc --import lib.o app.bzl` + a native run.
//
// BASE MESSAGES ARE LITERALS ON PURPOSE. Nothing releases a user exception
// subclass's inherited Message (#2951, the residual #2902 left), so a
// `base(<a +1 string producer>)` chain leaks it — measured 1492 B / 84
// allocations over 20 iterations of T4's shape. These cells are about LAYOUT,
// not lifetime, and an immortal literal keeps them independent of that open
// issue; T4 therefore runs generated-code ASan with LeakSanitizer OFF, so it
// still reports an out-of-bounds ACCESS while #2951's expected leak does not
// mask the result.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"  // #3156: the ONE sanitizer-honouring compileFile wrapper
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile2950 {
    std::string path;
    explicit TempFile2950(std::string p) : path(std::move(p)) {}
    ~TempFile2950() { std::remove(path.c_str()); }
};

void writeSource2950(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// -2 = the dependency toolchain is unavailable (test skips); -1 = the app did
// not build/link, or did not exit normally. A pre-fix COMPILE FAILURE lands on
// -1, which is what T3 observes: `e.Message` on an imported exception subclass
// did not compile at all before the fix.
int compileImportAndRun2950(const std::string& libSrc,
                            const std::string& appSrc,
                            bool sanitize = false) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2950_");
    TempFile2950 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2950 aObj((dir / (stem + "_a.o")).string());
    writeSource2950(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2950 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2950 bExe((dir / stem).string());
    writeSource2950(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    codegen::CodegenOptions opts;
    opts.sanitizeAddress = sanitize;
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps, opts).success)
        return -1;
    // #2951 (the leak this fix does NOT close) is expected on every exception
    // subclass, so LeakSanitizer is off here; AddressSanitizer's out-of-bounds
    // reporting — the part that is about THIS defect — stays on.
    std::string cmd = sanitize
        ? ("ASAN_OPTIONS=detect_leaks=0 " + bExe.path)
        : bExe.path;
    int status = std::system(cmd.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// T1 — THE ISSUE'S OWN REPRO, with an oracle. Both artifacts must read the
// SAME field: the importer through its own `%class.MyEx` GEP (`e.Val`), the
// exporter through the one it compiled (`e.ReadVal()`). §2.8.3 rule 4.
//
// Pre-fix, MEASURED: exit 3 — `e.Val` read 0 where the exporter's constructor
// had written 7, because the two sides disagreed by 32 bytes about where the
// field is. (`e.ReadVal()` read 1673780053, past the end of the 16-byte block.)
// ===========================================================================
BEZEL_TEST(xmod_imported_exception_subclass_layout_2950,
           imported_direct_exception_subclass_agrees_on_the_31_4_header) {
    const char* lib =
        "module ExLib2950T1; "
        "public class MyEx2950: Exception "
        "  public Val: int; "
        "  public constructor(n: int) base(\"mm\"); "
        "    Val := n; "
        "  end constructor; "
        "  public method ReadVal(): int "
        "    return Val; "
        "  end ReadVal; "
        "end MyEx2950;";
    int r = compileImportAndRun2950(
        lib,
        "module ExApp2950T1; import ExLib2950T1; "
        "public class C2950T1 "
        "  public static method Main(): int "
        "    using var e: MyEx2950 := new MyEx2950(7); "
        // The IMPORTER's own view of the field.
        "    if e.Val <> 7 then return 3; end if; "
        // The EXPORTER's compiled view of the same field.
        "    if e.ReadVal() <> 7 then return 4; end if; "
        "    return 0; "
        "  end Main; "
        "end C2950T1;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T2 — the two arms the module boundary has, in one program.
//
//   * An IMPORTED multi-level chain (`ExB2950: ExA2950: Exception`) allocated
//     by the importer. Pre-fix, MEASURED: both `b.A` and `b.B` read 0 (exit 3).
//   * A LOCAL class whose base is an IMPORTED exception subclass. Its own field
//     was fine — the importer laid that out and wrote it — but the INHERITED
//     one is written by the exporter's `bz$ExLib2950T2.ExA2950$ctor` through
//     the 48-byte layout, so pre-fix `l.A` read 0 (exit 5) while `l.L` read
//     105. That asymmetry is the tell: this arm is not "the imported class is
//     wrong", it is "the two artifacts disagree", and it stayed broken for the
//     same reason — the ancestor walk required an `ast::TypeDecl` on each step
//     and an imported ancestor has none.
// ===========================================================================
BEZEL_TEST(xmod_imported_exception_subclass_layout_2950,
           multi_level_imported_chain_and_local_class_over_an_imported_base) {
    const char* lib =
        "module ExLib2950T2; "
        "public class ExA2950: Exception "
        "  public A: int; "
        "  public constructor(n: int) base(\"ma\"); "
        "    A := n; "
        "  end constructor; "
        "end ExA2950; "
        "public class ExB2950: ExA2950 "
        "  public B: int; "
        "  public constructor(n: int) base(n); "
        "    B := n + 1; "
        "  end constructor; "
        "end ExB2950;";
    int r = compileImportAndRun2950(
        lib,
        "module ExApp2950T2; import ExLib2950T2; "
        "public class Local2950: ExA2950 "
        "  public L: int; "
        "  public constructor(n: int) base(n); "
        "    L := n + 100; "
        "  end constructor; "
        "end Local2950; "
        "public class C2950T2 "
        "  public static method Main(): int "
        "    using var b: ExB2950 := new ExB2950(7); "
        "    if b.A <> 7 then return 3; end if; "
        "    if b.B <> 8 then return 4; end if; "
        "    using var l: Local2950 := new Local2950(5); "
        "    if l.A <> 5 then return 5; end if; "
        "    if l.L <> 105 then return 6; end if; "
        "    return 0; "
        "  end Main; "
        "end C2950T2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T3 — the §31.4 MEMBER surface, which the same defect took away. An exported
// exception subclass lost its whole inherited surface across the boundary:
//
//     app.bzl:7:23: no member `Message` on type `MyEx` (§11.3, §12.1)
//
// while the identical declaration compiled fine as a local class. Pre-fix,
// MEASURED: exit -1 (the app does not compile), so this cell fails LOUDLY
// without the fix rather than reading a wrong value.
//
// Two things had to change for it, and they are the same fact: sema's own
// `Message` walk read the AST base list and required `b->decl` on each
// ancestor step exactly as codegen's layout gate did, AND its name set was a
// hand-listed positive list that had drifted from `kBuiltinExceptionHierarchy`
// (missing DisposedAccessException, SerializationException,
// ChannelClosedException, ChannelFullException, DomainFaultedException — all
// five of which the LAYOUT gate did carry). Both now ask
// `sema::builtinExceptionBaseOf`, so the member surface and the layout that
// backs it cannot disagree again. The `DisposedAccessException` arm below is
// the drift half, and it is LOCAL: it never needed the boundary to be wrong.
//
// The UNQUALIFIED `Message` arm is here because there were THREE copies of the
// walk, not two: the bare-identifier resolver
// (`ExprTypecheck_Exprs.cpp::inheritsBuiltinException`) had its own, with the
// same `b->decl` gate and its own drifted name list. Pre-fix a LOCAL class over
// an IMPORTED exception base got `unresolved identifier `Message`` for a bare
// `Message` in its own method body, where the identical hierarchy declared in
// ONE module resolves it.
//
// The bare spelling, not `this.Message`: `return this.Message;` inside a user
// exception subclass's own method fails to lower at all — `internal:
// return-value expression failed to lower to a value` then a SEGV on the null
// it returns — which is a pre-existing codegen defect measured identically on
// this commit's parent, filed separately. Using it here would pin that bug
// instead of this one.
// ===========================================================================
BEZEL_TEST(xmod_imported_exception_subclass_layout_2950,
           imported_exception_subclass_keeps_its_31_4_message_surface) {
    const char* lib =
        "module ExLib2950T3; "
        "public class MsgEx2950: Exception "
        "  public Val: int; "
        "  public constructor(n: int) base(\"mm\"); "
        "    Val := n; "
        "  end constructor; "
        "end MsgEx2950;";
    int r = compileImportAndRun2950(
        lib,
        "module ExApp2950T3; import ExLib2950T3; "
        // The drift arm: a LOCAL subclass of a built-in exception that the
        // hand-listed set omitted. Its layout always carried the header; only
        // `Message` was rejected.
        "public class LocalDae2950: DisposedAccessException "
        "  public D: int; "
        "  public constructor(n: int) base(\"dd\"); "
        "    D := n; "
        "  end constructor; "
        "end LocalDae2950; "
        // The third-copy arm: a LOCAL class over the IMPORTED exception base,
        // reading its inherited `Message` UNQUALIFIED from its own method body.
        "public class LocalOver2950: MsgEx2950 "
        "  public constructor(n: int) base(n); end constructor; "
        "  public method Bare(): string "
        "    return Message; "
        "  end Bare; "
        "end LocalOver2950; "
        "public class C2950T3 "
        "  public static method Main(): int "
        "    using var e: MsgEx2950 := new MsgEx2950(7); "
        "    if e.Message <> \"mm\" then return 3; end if; "
        "    if e.Val <> 7 then return 4; end if; "
        "    using var d: LocalDae2950 := new LocalDae2950(9); "
        "    if d.Message <> \"dd\" then return 5; end if; "
        "    if d.D <> 9 then return 6; end if; "
        "    using var o: LocalOver2950 := new LocalOver2950(3); "
        "    if o.Bare() <> \"mm\" then return 7; end if; "
        "    if o.Val <> 3 then return 8; end if; "
        "    return 0; "
        "  end Main; "
        "end C2950T3;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T4 — LOOP-AMPLIFIED, under generated-code AddressSanitizer, with the CONTROL
// that proves the predicate did not widen.
//
//   * `ArgEx2950: ArgumentException` uses the TWO-argument `base(message,
//     paramName)` overload, whose ParamName store lands at byte offset 32 —
//     16 bytes past the end of the pre-fix 16-byte block, 20 times, with a
//     churn allocation between iterations so the overwritten bytes are live
//     heap rather than slack.
//   * `Plain2950` is an imported class with NO exception base. It must keep
//     the two-slot `{ptr, i8, i32, i32}` layout on BOTH sides. If the fix had
//     widened the predicate — the failure shape a positive-list repair invites
//     — the importer would add a 32-byte header the exporter's compiled
//     constructor does not know about and `p.P`/`p.Q` would read wrong. This
//     control cell passes both with and without the fix BY DESIGN; it is here
//     to catch the over-correction, not the original defect.
//
// The printed total is the oracle, exact: 20 objects x (V=i) + (Message length
// 2 each) + (churn length 2 or 3) + (P=i) + (Q=i+1). A "fix" that changed the
// §31.4 rendering rather than the layout would fail here.
// ===========================================================================
BEZEL_TEST(xmod_imported_exception_subclass_layout_2950,
           two_arg_base_overload_amplified_and_the_no_header_control) {
    const char* lib =
        "module ExLib2950T4; "
        "public class ArgEx2950: ArgumentException "
        "  public V: int; "
        "  public constructor(n: int) base(\"mm\", \"pp\"); "
        "    V := n; "
        "  end constructor; "
        "end ArgEx2950; "
        "public class Plain2950 "
        "  public P: int; "
        "  public Q: int; "
        "  public constructor(n: int) "
        "    P := n; "
        "    Q := n + 1; "
        "  end constructor; "
        "end Plain2950;";
    int r = compileImportAndRun2950(
        lib,
        "module ExApp2950T4; import ExLib2950T4; "
        "public class C2950T4 "
        "  public static method Main(): int "
        "    var t: int := 0; "
        "    var i: int := 0; "
        "    while i < 20 do "
        "      i := i + 1; "
        "      using var e: ArgEx2950 := new ArgEx2950(i); "
        "      var churn: string := \"c\" + i.ToString(); "
        "      t := t + e.V + e.Message.Length + churn.Length; "
        "      using var p: Plain2950 := new Plain2950(i); "
        "      t := t + p.P + p.Q; "
        "    end while; "
        // sum(i)=210 (e.V) + 20*2 (Message \"mm\") + 51 (churn: 9x2 + 11x3)
        // + 210 (p.P) + 230 (p.Q) = 741.
        "    if t <> 741 then return 3; end if; "
        "    return 0; "
        "  end Main; "
        "end C2950T4;",
        /*sanitize=*/true);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);
}

}  // namespace bezel::module
