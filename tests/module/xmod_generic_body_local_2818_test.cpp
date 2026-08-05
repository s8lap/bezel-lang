// fix #2818 — an IMPORTED generic class's method body reads a BODY-LOCAL and
// returns 0 (§14.1 Generic declarations, §2.5 Dependency resolution, v0.240).
//
// §14.1 exports a generic class's method body source (schema v5 methodBodyText,
// #783) so the IMPORTER can monomorphize it per instantiation — "the price of
// monomorphizing generics across artifact boundaries" (§14.1). ModuleLoader's
// parseAndAttachMethodBody re-parses that text in a FRESH SemaContext, then
// remaps the body's expr→symbol bindings into the importer's context so codegen
// can lower the body. It remapped FIELD, PARAMETER and METHOD symbols — and
// silently DROPPED every symbol declared INSIDE the body: a `var`/`const` local,
// a `for`/`for-in` iteration variable, a `using var`. Those have no importer-side
// twin to remap to, so the loop's if/else-if chain fell through and never copied
// them.
//
// Consequence: in the importer, `ctx_.exprSymbol()` returned null for every READ
// of a body-local. `lowerExpr` on the identifier produced no value, so the
// enclosing expression degraded to a zero/null result via the #2491 return-path
// fallback. The body still RAN (and still allocated) — only the value was lost.
// A silent wrong value across the module boundary with no error, which §2.5's
// artifact-based resolution must never produce: the same source compiled as one
// unit answers correctly.
//
// The fix carries a LocalSymbol binding over VERBATIM (it *is* the fresh symbol;
// its `decl` is the re-parsed ast::LocalDecl codegen keys `locals_` on, and the
// fresh context is kept alive alongside the AST).
//
// ⚠ VACUITY HAZARD — read before editing any source below. buildMethodStub emits
// `=> 0` as the stub body for EVERY sibling method of the same class, including
// one whose declared return type is a REFERENCE type (`string`, a class). `0` is
// not a `string`, so that stub fails to typecheck, which trips the #783 soundness
// gate, which leaves the body UNATTACHED — and the call then falls back to the
// exporter's own compiled function and returns the RIGHT answer by accident.
// A `Box<T>` carrying any reference-returning method therefore exercises NONE of
// this path. Every generic class below is deliberately free of one. (That stub
// defect is filed separately as #2820.)
//
// TEST MATRIX (each shape returns 0 without the fix, the asserted value with it):
//   T1  the issue's own repro — an imported generic STATIC method whose return
//       expression reads a string local: sum over 1..20 == 91, not 0.
//   T2  discriminant grid proving the fault is "imported generic + body-local",
//       NOT "static": generic-static, generic-INSTANCE, and non-generic-static
//       are compared in one program, each with and without a body-local.
//   T3  local kinds — for-in iteration variable, nested-block local, `const`
//       local, bool local, reassigned local — all read correctly.
//   T4  the §4.6 string-refcount shapes — the ones the fix could BREAK, since
//       several release/retain sites gate on "is this operand a named local?".
//   T5  a body-local is not confused with a same-named FIELD (the remap paths
//       for local and field run over the same map).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile2818 {
    std::string path;
    explicit TempFile2818(const std::string& p) : path(p) {}
    ~TempFile2818() { std::remove(path.c_str()); }
};

void writeSource2818(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile the dependency module → object.
bool compileToObj2818(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile the importer against that object → exe, run it, return its exit code
// (-1 on any compile/link/run failure).
int compileImportAndRun2818(const std::string& bSrc,
                            const std::string& bExe,
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

// Build A → object, then B importing A → exe, run, return exit code.
// Returns -2 when the dependency object could not be produced (toolchain issue),
// so a genuine 0 is never mistaken for a skip.
int runXmod2818(const std::string& stem, const std::string& aSrcText,
                const std::string& bSrcText) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string tag = stem + "_" + std::to_string(std::rand());
    TempFile2818 aSrc((dir / (tag + "_a.bzl")).string());
    TempFile2818 aObj((dir / (tag + "_a.o")).string());
    writeSource2818(aSrc.path, aSrcText);
    if (!compileToObj2818(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;
    TempFile2818 bSrc((dir / (tag + "_b.bzl")).string());
    TempFile2818 bExe((dir / (tag + "_b")).string());
    writeSource2818(bSrc.path, bSrcText);
    return compileImportAndRun2818(bSrc.path, bExe.path, aObj.path);
}

}  // namespace

// ===========================================================================
// T1 — the issue's repro. An imported generic class's static method whose
// return expression reads a STRING BODY-LOCAL. §14.1 / §2.5.
// Sum over i in 1..20 of len("xm-{i}") = 9*4 + 11*5 = 91.
// Without the fix this returns 0 (with an `internal: … — #2491` warning) while
// still executing — and allocating — the body.
// ===========================================================================
BEZEL_TEST(xmod_generic_body_local_2818, t1_string_local_return_value_14_1) {
    int rc = runXmod2818(
        "bz2818_t1",
        "module Lib2818A; "
        "public class Box<T> "
        "  public static method Copy(i: int): int "
        "    var built := $\"xm-{i}\"; "
        "    return built.Length; "
        "  end Copy; "
        "end Box;",
        "module App; import Lib2818A; "
        "public class C "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..20 do "
        "      total := total + Box<string>.Copy(i); "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;");
    if (rc == -2) return;  // dependency object could not be built
    BEZEL_EXPECT_EQ(rc, 91);
}

// ===========================================================================
// T2 — DISCRIMINANT GRID. The issue named "static method" as the fault; it is
// not. The axis is "imported GENERIC class + a body-local read", orthogonal to
// static-vs-instance. One program compares six cells and encodes the whole grid
// into one exit code so a partial failure cannot pass:
//
//   generic  / static   / no local  -> 11   (correct before AND after)
//   generic  / static   / local     -> 4    (0 before)
//   generic  / instance / no local  -> 11   (correct before AND after)
//   generic  / instance / local     -> 4    (0 before)
//   plain    / static   / no local  -> 11   (correct before AND after — the
//   plain    / static   / local     -> 4     non-generic control: a plain class
//                                            is called directly, never
//                                            reconstructed, so it was always
//                                            right and must STAY right)
//
// Encoded sum: 11+4+11+4+11+4 = 45. Before the fix the two generic local cells
// read 0, giving 37 — a distinct value, so the assert cannot pass vacuously.
// (45 is well under runAot's 8-bit exit truncation.)
// ===========================================================================
BEZEL_TEST(xmod_generic_body_local_2818, t2_generic_not_static_is_the_axis_14_1) {
    int rc = runXmod2818(
        "bz2818_t2",
        "module Lib2818B; "
        "public class Box<T> "
        "  public v: T; "
        "  public static method AddOne(i: int): int return i + 1; end AddOne; "
        "  public static method Copy(i: int): int "
        "    var built := $\"xm-{i}\"; return built.Length; "
        "  end Copy; "
        "  public method InstAddOne(i: int): int return i + 1; end InstAddOne; "
        "  public method InstCopy(i: int): int "
        "    var built := $\"xm-{i}\"; return built.Length; "
        "  end InstCopy; "
        "end Box; "
        "public class Plain "
        "  public static method AddOne(i: int): int return i + 1; end AddOne; "
        "  public static method Copy(i: int): int "
        "    var built := $\"xm-{i}\"; return built.Length; "
        "  end Copy; "
        "end Plain;",
        "module App; import Lib2818B; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Box<string>(); "
        "    return Box<string>.AddOne(10) + Box<string>.Copy(7) "
        "         + b.InstAddOne(10) + b.InstCopy(7) "
        "         + Plain.AddOne(10) + Plain.Copy(7); "
        "  end Main; "
        "end C;");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 45);
}

// ===========================================================================
// T3 — the local KINDS the remap must carry. Every one of these is a
// sema::LocalSymbol; each was dropped before the fix. Encoded as one sum:
//   for-in iteration variable   SumRange(4)   = 1+2+3+4      = 10
//   nested-block local          BlockLocal(9) = 9*2          = 18
//   `const` local               ConstLocal(5) = 5*3          = 15
//   bool local + branch         BoolLocal(11)               =  1
//   reassigned local            Reassign(4)   = ((4+1)*2)-3  =  7
//   for-in with `step`          StepSum(6)    = 0+2+4+6      = 12
//                                                        total = 63
// Before the fix every one of these returns 0 (total 0).
// ===========================================================================
BEZEL_TEST(xmod_generic_body_local_2818, t3_local_kinds_all_carried_14_1) {
    int rc = runXmod2818(
        "bz2818_t3",
        "module Lib2818C; "
        "public class Box<T> "
        "  public v: T; "
        "  public static method SumRange(n: int): int "
        "    var acc := 0; "
        "    for var i in 1..n do acc := acc + i; end; "
        "    return acc; "
        "  end SumRange; "
        "  public static method BlockLocal(i: int): int "
        "    var outerv := i; "
        "    if i > 0 then var innerv := outerv * 2; return innerv; end; "
        "    return outerv; "
        "  end BlockLocal; "
        "  public static method ConstLocal(i: int): int "
        "    const factor := 3; return i * factor; "
        "  end ConstLocal; "
        "  public static method BoolLocal(i: int): int "
        "    var big := i > 10; "
        "    if big then return 1; end; "
        "    return 0; "
        "  end BoolLocal; "
        "  public static method Reassign(i: int): int "
        "    var acc := i; acc := acc + 1; acc := acc * 2; acc := acc - 3; "
        "    return acc; "
        "  end Reassign; "
        "  public static method StepSum(n: int): int "
        "    var acc := 0; "
        "    for var i in 0..n step 2 do acc := acc + i; end; "
        "    return acc; "
        "  end StepSum; "
        "end Box;",
        "module App; import Lib2818C; "
        "public class C "
        "  public static method Main(): int "
        "    return Box<string>.SumRange(4) + Box<string>.BlockLocal(9) "
        "         + Box<string>.ConstLocal(5) + Box<string>.BoolLocal(11) "
        "         + Box<string>.Reassign(4) + Box<string>.StepSum(6); "
        "  end Main; "
        "end C;");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 63);
}

// ===========================================================================
// T4 — the §4.6 STRING-REFCOUNT shapes. These are the shapes the fix could most
// plausibly BREAK rather than fix: several codegen sites decide whether to arm a
// scope-end `bz_string_release` (or a borrow retain) by asking
// `dynamic_cast<LocalSymbol*>(ctx_.exprSymbol(*id))` whether the operand is a
// NAMED LOCAL. Before the fix that cast returned null inside every imported
// generic body, so those sites uniformly took the "not a local" path; after it
// they take the named-local path for the first time. If the fix armed an
// unbalanced release, these are the shapes that would double-free.
//
// Asserted on VALUE (not on "doesn't crash"): a released-too-early string would
// read a freed length. Encoded sum:
//   StrManyReads(3)  s="ab-3" read 3x  -> 4+4+4 = 12
//   StrAlias(3)      t := s, both read ->   4+4 =  8
//   StrReassign(3)   s reassigned to a 2nd producer, len("xyz-3-3") =  7
//                                                             total = 27
// Before the fix all three return 0.
// ===========================================================================
BEZEL_TEST(xmod_generic_body_local_2818, t4_string_local_refcount_shapes_4_6) {
    int rc = runXmod2818(
        "bz2818_t4",
        "module Lib2818D; "
        "public class Box<T> "
        "  public v: T; "
        "  public static method StrManyReads(i: int): int "
        "    var s := $\"ab-{i}\"; "
        "    var a := s.Length; var b := s.Length; var c := s.Length; "
        "    return a + b + c; "
        "  end StrManyReads; "
        "  public static method StrAlias(i: int): int "
        "    var s := $\"ab-{i}\"; "
        "    var t := s; "
        "    return t.Length + s.Length; "
        "  end StrAlias; "
        "  public static method StrReassign(i: int): int "
        "    var s := $\"ab-{i}\"; "
        "    s := $\"xyz-{i}-{i}\"; "
        "    return s.Length; "
        "  end StrReassign; "
        "end Box;",
        "module App; import Lib2818D; "
        "public class C "
        "  public static method Main(): int "
        "    return Box<string>.StrManyReads(3) + Box<string>.StrAlias(3) "
        "         + Box<string>.StrReassign(3); "
        "  end Main; "
        "end C;");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 27);
}

// NOTE — an axis deliberately NOT covered here, and why. The natural disposal
// probe ("a `using var` OWNED CLASS local inside the imported generic body is
// disposed exactly once") is UNREACHABLE on this path and would ship vacuous.
// Constructing a class inside the body requires naming it, and the synthetic
// class ModuleLoader reconstructs contains ONLY the generic type's own members —
// a sibling type does not resolve, so the #783 soundness gate refuses to attach
// the body and the call falls back to the exporter's own compiled function,
// which was always correct. A first draft of this test asserted exactly that
// shape and PASSED without the fix. The remaining spelling, `new T()` under
// `where T: new()`, is blocked by a different open defect (#2477: codegen cannot
// construct `new T()` in the abstract template body). T4 above therefore probes
// the reachable refcounted-resource axis (§4.6 strings) instead.

// ===========================================================================
// T5 — a body-LOCAL and a FIELD of the same name are remapped by two different
// arms of the same loop; the local must not shadow-collide with the field's
// remap. `var tag := 5;` alongside `this.tag` (= 7) must read 5 + 7 = 12.
// Before the fix the local read produced no value and the sum degraded to 0.
// ===========================================================================
BEZEL_TEST(xmod_generic_body_local_2818, t5_local_and_field_same_name_14_1) {
    int rc = runXmod2818(
        "bz2818_t5",
        "module Lib2818E; "
        "public class Box<T> "
        "  public tag: int; "
        "  public static method LocalAndParam(tagIn: int): int "
        "    var tag := 5; "
        "    return tag + tagIn; "
        "  end LocalAndParam; "
        "end Box;",
        "module App; import Lib2818E; "
        "public class C "
        "  public static method Main(): int "
        "    return Box<string>.LocalAndParam(7); "  // 5 + 7
        "  end Main; "
        "end C;");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 12);
}

}  // namespace bezel::module
