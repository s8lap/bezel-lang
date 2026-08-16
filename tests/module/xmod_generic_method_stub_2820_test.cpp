// fix #2820 — `buildMethodStub` emitted `=> 0` for EVERY return type, so ONE
// sibling whose return type `0` does not fit silently disabled §14.1/#783 body
// reconstruction for the WHOLE imported generic class.
// (§14.1 Generic declarations, §2.5 Dependency resolution, v0.240.)
//
// When the importer monomorphizes an imported generic class's method body (#783,
// schema v5 methodBodyText), ModuleLoader re-parses that body inside a synthetic
// single-class source in which every OTHER method appears as a signature stub.
// The stub body was `=> 0` regardless of the declared return type (only `bool`
// was special-cased). `0` does not typecheck against `string`, a class, `owned
// C`, an interface, `Task<…>` — nor, per §5.1, against `byte`, `sbyte`, `short`,
// `ushort`, `uint`, `ulong` or `char`, which reject an `int` literal outright.
// The fresh context then reported an error, and parseAndAttachMethodBody's #783
// soundness gate refused to attach THE BODY BEING RECONSTRUCTED — even though the
// failure was entirely in an unrelated sibling's synthetic stub.
//
// Net effect: declaring one such method anywhere in an imported generic class
// disabled cross-artifact monomorphization for every method of that class, on a
// condition no author can see. With #2821's layout gate in the tree that now
// surfaces as a REJECTED compile (before #2821 it was a silent wrong value).
//
// The fix emits a body that is type-correct BY CONSTRUCTION for any signature —
// a self-call forwarding the stub's own parameters — rather than enumerating the
// return types that happen to have a literal (the #2787/#2790 lesson). The stub
// is never lowered: parseAndAttach* runs collectSymbols / resolveTypes /
// typecheckExpressions on the synthetic module and nothing else.
//
// ⚠ VACUITY HAZARD — read before editing any source below. The obvious test for
// this bug does NOT work. When reconstruction is disabled the call falls back to
// the EXPORTER's own compiled function, and for most bodies that function returns
// the RIGHT answer — so a program whose only defect is "the body was not
// reconstructed" passes without the fix. (The #2818 lane's first adversarial grid
// and first test draft both passed on its base for exactly this reason.) Every
// hinge below therefore uses a class whose layout SHIFTS under the instantiation
// (`v: T` declared before `tag: int`), where the fallback is not merely
// unnecessary but unsound — so without the fix the compile is REJECTED by the
// §14.1/#783/#2821 gate, and with it the body reconstructs and the program runs.
// The assertion is the RUN's exit value, never "it compiled".
//
// TEST MATRIX (each hinge: compile fails without the fix, returns 7 with it):
//   T1  a `string`-returning sibling — the issue's own repro shape.
//   T2  narrow/unsigned integer siblings (`byte`, `uint`, `ulong`, `char`) —
//       proves the defect was never about reference types, but about `0` not
//       fitting the declared return type at all (§5.1).
//   T3  an `owned`-returning sibling (no literal of that type can exist).
//   T4  the reconstructed body CALLS the reference-returning sibling and uses
//       its result (`Name().Length + tag`): the stub must carry the right RESULT
//       TYPE, not merely typecheck.
//   T5  CONTROL: a class with the same reference-returning sibling but NO
//       type-parameter-typed storage — already worked, must still return 7.
//   T6  CONTROL: the CONSTRUCTOR-body path (buildSyntheticClassSource uses the
//       same stub builder) with a reference-returning sibling called from the
//       ctor body — the field initialization must still be correct.
//
// KNOWN RESIDUAL, deliberately not fixed here: a stub whose SIGNATURE names
// another type of the exporting module (`MakeCell(): owned Cell`) still fails,
// because that type name is not declared in the single-class synthetic source at
// all — the #710 / #2290-bug-2 "sibling-type reference" family, not the `=> 0`
// body defect this fixes. Filed separately.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile2820 {
    std::string path;
    explicit TempFile2820(std::string p) : path(std::move(p)) {}
    ~TempFile2820() { std::remove(path.c_str()); }
};

void writeSource2820(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2820(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

int compileImportAndRun2820(const std::string& bSrc, const std::string& bExe,
                            const std::string& aObj) {
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj;
    d.objectPath = aObj;
    deps.push_back(d);
    if (!driver.compileFile(bSrc, bExe, deps).success) return -1;
    int status = std::system(bExe.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// Build A → object, then B importing A → exe, run it, return the exit code.
// -1 on any compile/link/run failure; -2 when the dependency object itself could
// not be produced, so a toolchain problem is never read as a genuine 0.
int runXmod2820(const std::string& stem, const std::string& aSrcText,
                const std::string& bSrcText) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string tag = stem + "_" + unique_stem("");
    TempFile2820 aSrc((dir / (tag + "_a.bzl")).string());
    TempFile2820 aObj((dir / (tag + "_a.o")).string());
    writeSource2820(aSrc.path, aSrcText);
    if (!compileToObj2820(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;
    TempFile2820 bSrc((dir / (tag + "_b.bzl")).string());
    TempFile2820 bExe((dir / (tag + "_b")).string());
    writeSource2820(bSrc.path, bSrcText);
    return compileImportAndRun2820(bSrc.path, bExe.path, aObj.path);
}

}  // namespace

// ===========================================================================
// T1 — a `string`-returning sibling. Its `=> 0` stub failed to typecheck, which
// tripped the #783 gate, which left `GetTag`'s body unattached — and `GetTag` is
// on a class whose layout shifts at `<string>`, so the fallback is unsound and
// the compile is rejected. §14.1 / §2.5.
// ===========================================================================
BEZEL_TEST(xmod_generic_method_stub_2820,
           t1_reference_returning_sibling_no_longer_blocks_14_1) {
    BEZEL_EXPECT_EQ(7, runXmod2820("bz2820_t1",
        "module Lib2820A; "
        "public class Box2820A<T> "
        "  public v: T; "
        "  public tag: int; "
        "  public method GetTag(): int "
        "    return this.tag; "
        "  end GetTag; "
        "  public method Name(): string "
        "    return \"box\"; "
        "  end Name; "
        "end Box2820A;",
        "module App2820A; import Lib2820A; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Box2820A<string>(); "
        "    b.v := \"hello\"; "
        "    b.tag := 7; "
        "    return b.GetTag(); "
        "  end Main; "
        "end C;"));
}

// ===========================================================================
// T2 — narrow/unsigned integer siblings. `byte`, `uint`, `ulong` and `char` all
// reject a bare `int` literal (§5.1), so each one alone disabled reconstruction
// exactly as a reference return did. This pins that the defect was the literal,
// not the reference-ness. §14.1 / §5.1.
// ===========================================================================
BEZEL_TEST(xmod_generic_method_stub_2820,
           t2_narrow_and_unsigned_integer_siblings_no_longer_block_14_1) {
    BEZEL_EXPECT_EQ(7, runXmod2820("bz2820_t2",
        "module Lib2820B; "
        "public class Box2820B<T> "
        "  public v: T; "
        "  public tag: int; "
        "  public method GetTag(): int "
        "    return this.tag; "
        "  end GetTag; "
        "  public method B(x: byte): byte "
        "    return x; "
        "  end B; "
        "  public method U(): uint "
        "    return 1U; "
        "  end U; "
        "  public method L(): ulong "
        "    return 1UL; "
        "  end L; "
        "  public method Ch(): char "
        "    return 'x'; "
        "  end Ch; "
        "end Box2820B;",
        "module App2820B; import Lib2820B; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Box2820B<string>(); "
        "    b.v := \"hello\"; "
        "    b.tag := 7; "
        "    return b.GetTag(); "
        "  end Main; "
        "end C;"));
}

// ===========================================================================
// T3 — an `owned`-returning sibling. No literal of an owned class type exists at
// all, which is why a per-return-type literal table could not have fixed this.
// §14.1 / §28.
// ===========================================================================
BEZEL_TEST(xmod_generic_method_stub_2820,
           t3_owned_returning_sibling_no_longer_blocks_14_1) {
    BEZEL_EXPECT_EQ(7, runXmod2820("bz2820_t3",
        "module Lib2820C; "
        "public class Box2820C<T> "
        "  public v: T; "
        "  public tag: int; "
        "  public method GetTag(): int "
        "    return this.tag; "
        "  end GetTag; "
        "  public static method Clone(): owned Box2820C<T> "
        "    return new Box2820C<T>(); "
        "  end Clone; "
        "end Box2820C;",
        "module App2820C; import Lib2820C; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Box2820C<string>(); "
        "    b.v := \"hello\"; "
        "    b.tag := 7; "
        "    return b.GetTag(); "
        "  end Main; "
        "end C;"));
}

// ===========================================================================
// T4 — the reconstructed body CALLS the reference-returning sibling and consumes
// its result. A stub that merely parsed would not be enough: the self-call's
// RESULT TYPE has to be the declared `string` for `.Length` to resolve.
// 2 + "boxed".Length(5) == 7. §14.1 / §22.6.
// ===========================================================================
BEZEL_TEST(xmod_generic_method_stub_2820,
           t4_self_call_to_sibling_keeps_its_result_type_14_1) {
    BEZEL_EXPECT_EQ(7, runXmod2820("bz2820_t4",
        "module Lib2820D; "
        "public class Box2820D<T> "
        "  public v: T; "
        "  public tag: int; "
        "  public method Name(): string "
        "    return \"boxed\"; "
        "  end Name; "
        "  public method NameLen(): int "
        "    return this.Name().Length + this.tag; "
        "  end NameLen; "
        "end Box2820D;",
        "module App2820D; import Lib2820D; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Box2820D<string>(); "
        "    b.v := \"hello\"; "
        "    b.tag := 2; "
        "    return b.NameLen(); "
        "  end Main; "
        "end C;"));
}

// ===========================================================================
// T5 — CONTROL: the same reference-returning sibling on a class with NO
// type-parameter-typed storage. Its layout cannot shift, so this shape compiled
// and ran correctly before the fix too; it must keep doing so. §14.1 / §2.5.
// ===========================================================================
BEZEL_TEST(xmod_generic_method_stub_2820,
           t5_control_unshifted_class_still_runs_2_5) {
    BEZEL_EXPECT_EQ(7, runXmod2820("bz2820_t5",
        "module Lib2820E; "
        "public class Flat2820E<T> "
        "  public tag: int; "
        "  public method GetTag(): int "
        "    return this.tag; "
        "  end GetTag; "
        "  public method Name(): string "
        "    return \"flat\"; "
        "  end Name; "
        "end Flat2820E;",
        "module App2820E; import Lib2820E; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Flat2820E<string>(); "
        "    b.tag := 7; "
        "    return b.GetTag(); "
        "  end Main; "
        "end C;"));
}

// ===========================================================================
// T6 — CONTROL: the CONSTRUCTOR-body path. buildSyntheticClassSource builds its
// stubs with the same helper, and its ctor body here calls the
// reference-returning sibling, so the stub's shape has to keep that self-call
// resolvable and correctly typed. 5 + "ab".Length(2) == 7. §14 / §22.7.
// ===========================================================================
BEZEL_TEST(xmod_generic_method_stub_2820,
           t6_control_ctor_body_self_call_still_resolves_14_1) {
    BEZEL_EXPECT_EQ(7, runXmod2820("bz2820_t6",
        "module Lib2820F; "
        "public class Box2820F<T> "
        "  public v: T; "
        "  public tag: int; "
        "  public constructor(n: int) "
        "    tag := n + this.Label().Length; "
        "  end constructor; "
        "  public method Label(): string "
        "    return \"ab\"; "
        "  end Label; "
        "end Box2820F;",
        "module App2820F; import Lib2820F; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Box2820F<string>(5); "
        "    b.v := \"hello\"; "
        "    return b.tag; "
        "  end Main; "
        "end C;"));
}

}  // namespace bezel::module
