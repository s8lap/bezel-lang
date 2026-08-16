// Cross-module imported GENERIC METHOD returning a GENERIC RECORD (#1937 / #428
// family), §14.1 monomorphization, §2.5 modules, §2.8.3 rule 2 "external
// instantiation behaves identically".
//
// A generic *method* — one with its OWN `<A, B>` type parameters, on an ordinary
// (non-generic) class — that returns a generic RECORD parameterized by those
// method type parameters silently miscompiles when called from another module:
//
//   module Lib;
//   public record Pair<A, B>(F: A, S: B);
//   public class Maker
//     public static method Make<A, B>(a: A, b: B): Pair<A, B>
//       where A: value where B: value  return new Pair<A, B>(a, b);
//   end Maker;
//
//   // importer:  var p := Lib.Maker.Make<long, long>(10, 20);
//   //            p.F / p.S  -->  SILENTLY MISSING  (register-return Pair)
//   //   with a wide `Triple<A,B,C>` return  -->  SEGFAULT on a layout touch (sret)
//
// Root cause: the defining module compiles the generic method's body against the
// i32 placeholder; unlike a generic CLASS's methods (#783, exported as
// methodBodyText and reconstructed importer-side), a generic METHOD on a
// non-generic class has NO reconstructed body, so codegen falls through to the
// external i32-placeholder template and the concrete `Pair<long,long>` /
// `Triple<...>` return crosses the boundary at the wrong width/layout. It hits
// BOTH the register-return (≤16 byte Pair) and sret (>16 byte Triple) ABI
// classes, so it is NOT the #1894/#1895 return-ABI-width bug (fixed & pinned).
//
// The importer-side monomorphization that would make this CORRECT is the #428
// core (it requires reconstructing a body that constructs a sibling generic
// record). Until that lands, the shape is FAIL-CLOSED: sema rejects the call
// with a clean §14.1/§2.8.3/#1937 diagnostic instead of emitting a binary that
// silently drops the value or crashes. A clean compile-error is strictly better
// than a silent wrong value.
//
// TEST MATRIX:
//   T1  register-return Pair<long,long>: Make<long,long>(...) is REJECTED
//       (was: compiled & silently dropped the fields).
//   T2  sret Triple<long,long,long>: Make<long,long,long>(...) is REJECTED
//       (was: compiled & segfaulted on `.X.ToString().Length`).
//   T3  even the placeholder-matching Pair<int,int> instantiation is REJECTED —
//       it too was silently broken (fields dropped, `p.F` typed `?`), so a clean
//       reject is an improvement, and the gate does not rely on i32-luck.
//   T4  CONTROL (must still COMPILE & RUN): direct cross-module construction of
//       the imported generic record `new Lib.Pair<int,int>(...)` + field access —
//       unaffected by the gate (returns 30 = 10 + 20).
//   T5  CONTROL (must still COMPILE & RUN): an imported generic method whose
//       return type does NOT use its type parameter (`Count<T>(x:T): int`) — the
//       gate's return-type walk returns false, so it is left alone (returns 7).

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

struct TempFile1937 {
    std::string path;
    explicit TempFile1937(std::string p) : path(std::move(p)) {}
    ~TempFile1937() { std::remove(path.c_str()); }
};

void writeSource1937(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile A → object. Returns true on success.
bool compileToObj1937(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile B importing A → exe, run, return exit code (-1 on compile/link/run
// failure). Used by the positive-control (must-run) cases.
int compileImportAndRun1937(const std::string& bSrc, const std::string& bExe,
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

// True when compiling B importing A FAILS (the fail-closed gate fires). This is
// the fail-without/pass-with hinge: WITHOUT the #1937 gate the compile SUCCEEDS
// (and the emitted binary silently miscompiles), so this returns false and the
// test fails; WITH the gate the compile is a clean reject and this returns true.
bool compileImportFails1937(const std::string& bSrc, const std::string& bExe,
                            const std::string& aObj) {
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj;
    d.objectPath = aObj;
    deps.push_back(d);
    return !driver.compileFile(bSrc, bExe, deps).success;
}

// Library used by T1/T3/T4: a register-return Pair and a Maker with a generic
// Make method returning it.
const char* kLibPair1937 =
    "module Lib1937P; "
    "public record Pair<A, B>(F: A, S: B); "
    "public class Maker "
    "  public static method Make<A, B>(a: A, b: B): Pair<A, B> "
    "    where A: value where B: value "
    "    return new Pair<A, B>(a, b); "
    "  end Make; "
    "end Maker;";

}  // namespace

// ===========================================================================
// T1 — register-return Pair<long,long>: the cross-module generic-method-return
// call is REJECTED (was: compiled to a binary that silently dropped p.F / p.S).
// ===========================================================================
BEZEL_TEST(xmod_generic_method_record_return_1937,
           t1_register_pair_long_rejected_14_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz1937_t1_");
    TempFile1937 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile1937 aObj((dir / (stem + "_a.o")).string());
    writeSource1937(aSrc.path, kLibPair1937);
    BEZEL_EXPECT_TRUE(compileToObj1937(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    // The importer body is the issue's exact repro: WITHOUT the #1937 gate it
    // compiles cleanly (exit 0) and silently drops the two field lines; WITH the
    // gate the call is a clean sema reject. `p.F.ToString()` type-checks even
    // when the pre-gate sema mis-typed `p.F` as `?` (ToString is an Object
    // method), so pre-gate this really did compile — the fail-without hinge.
    std::string bSrc =
        "module App1937P; import Lib1937P; "
        "public class C "
        "  public static method Main(): int "
        "    var p := Lib1937P.Maker.Make<long, long>(10, 20); "
        "    Console.WriteLine(p.F.ToString()); "
        "    Console.WriteLine(p.S.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
    TempFile1937 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile1937 bExe((dir / (stem + "_b")).string());
    writeSource1937(bSrcFile.path, bSrc);
    BEZEL_EXPECT_TRUE(
        compileImportFails1937(bSrcFile.path, bExe.path, aObj.path));
}

// ===========================================================================
// T2 — sret Triple<long,long,long>: the wide (>16 byte) generic-record return is
// REJECTED (was: compiled and SEGFAULTED at runtime on a layout touch).
// ===========================================================================
BEZEL_TEST(xmod_generic_method_record_return_1937,
           t2_sret_triple_long_rejected_14_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz1937_t2_");
    TempFile1937 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile1937 aObj((dir / (stem + "_a.o")).string());
    writeSource1937(aSrc.path,
        "module Lib1937T; "
        "public record Triple<A, B, C>(X: A, Y: B, Z: C); "
        "public class Maker "
        "  public static method Make<A, B, C>(a: A, b: B, c: C): Triple<A, B, C> "
        "    where A: value where B: value where C: value "
        "    return new Triple<A, B, C>(a, b, c); "
        "  end Make; "
        "end Maker;");
    BEZEL_EXPECT_TRUE(compileToObj1937(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    // Pre-gate: this compiled and SEGFAULTED at runtime on the sret layout
    // touch; `t.X.ToString()` type-checks pre-gate (mis-typed `?`, Object
    // method), so the compile succeeded — the fail-without hinge. Post-gate the
    // Make<...> call is a clean reject.
    std::string bSrc =
        "module App1937T; import Lib1937T; "
        "public class C "
        "  public static method Main(): int "
        "    var t := Lib1937T.Maker.Make<long, long, long>(10, 20, 30); "
        "    Console.WriteLine(t.X.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
    TempFile1937 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile1937 bExe((dir / (stem + "_b")).string());
    writeSource1937(bSrcFile.path, bSrc);
    BEZEL_EXPECT_TRUE(
        compileImportFails1937(bSrcFile.path, bExe.path, aObj.path));
}

// ===========================================================================
// T3 — even the placeholder-matching Pair<int,int> instantiation is REJECTED.
// It too was silently broken (fields dropped; `p.F` typed `?`), so the gate
// uniformly rejects the shape rather than relying on i32-representation luck.
// ===========================================================================
BEZEL_TEST(xmod_generic_method_record_return_1937,
           t3_register_pair_int_also_rejected_14_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz1937_t3_");
    TempFile1937 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile1937 aObj((dir / (stem + "_a.o")).string());
    writeSource1937(aSrc.path, kLibPair1937);
    BEZEL_EXPECT_TRUE(compileToObj1937(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    // Pre-gate: compiled (exit 0) and silently dropped the field line even for
    // this placeholder-matching `<int,int>` instantiation (the returned record's
    // value never crossed the boundary). Post-gate: clean reject.
    std::string bSrc =
        "module App1937I; import Lib1937P; "
        "public class C "
        "  public static method Main(): int "
        "    var p := Lib1937P.Maker.Make<int, int>(10, 20); "
        "    Console.WriteLine(p.F.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
    TempFile1937 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile1937 bExe((dir / (stem + "_b")).string());
    writeSource1937(bSrcFile.path, bSrc);
    BEZEL_EXPECT_TRUE(
        compileImportFails1937(bSrcFile.path, bExe.path, aObj.path));
}

// ===========================================================================
// T4 — CONTROL: direct cross-module construction of the imported generic record
// + field access still COMPILES & RUNS (unaffected by the gate). Returns 30.
// ===========================================================================
BEZEL_TEST(xmod_generic_method_record_return_1937,
           t4_direct_record_construction_still_works_9_4) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz1937_t4_");
    TempFile1937 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile1937 aObj((dir / (stem + "_a.o")).string());
    writeSource1937(aSrc.path, kLibPair1937);
    BEZEL_EXPECT_TRUE(compileToObj1937(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App1937D; import Lib1937P; "
        "public class C "
        "  public static method Main(): int "
        "    var p := new Lib1937P.Pair<int, int>(10, 20); "
        "    return p.F + p.S; "  // 30
        "  end Main; "
        "end C;";
    TempFile1937 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile1937 bExe((dir / (stem + "_b")).string());
    writeSource1937(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(
        compileImportAndRun1937(bSrcFile.path, bExe.path, aObj.path), 30);
}

// ===========================================================================
// T5 — CONTROL: an imported generic method whose return type does NOT use its
// type parameter is NOT rejected — it still COMPILES & RUNS. Returns 7.
// ===========================================================================
BEZEL_TEST(xmod_generic_method_record_return_1937,
           t5_nonparam_return_generic_method_still_works_14_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz1937_t5_");
    TempFile1937 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile1937 aObj((dir / (stem + "_a.o")).string());
    writeSource1937(aSrc.path,
        "module Lib1937C; "
        "public class Util "
        "  public static method Count<T>(x: T): int "
        "    where T: value "
        "    return 7; "
        "  end Count; "
        "end Util;");
    BEZEL_EXPECT_TRUE(compileToObj1937(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App1937C; import Lib1937C; "
        "public class C "
        "  public static method Main(): int "
        "    return Lib1937C.Util.Count<long>(99); "  // 7
        "  end Main; "
        "end C;";
    TempFile1937 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile1937 bExe((dir / (stem + "_b")).string());
    writeSource1937(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(
        compileImportAndRun1937(bSrcFile.path, bExe.path, aObj.path), 7);
}

}  // namespace bezel::module
