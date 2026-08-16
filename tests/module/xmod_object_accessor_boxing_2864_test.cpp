// Spec-derived regression test for issue #2864 — the LIBRARY-BUILD half.
//
// §8.6 "Boxing" / §23.2 "Expression-bodied read-only properties" /
// §23.4 "Indexers" / §2.5 "Dependency resolution", v0.240.
//
// The single-module half lives in
// tests/codegen/named_object_property_boxing_2864_test.cpp. This file exists
// because ONE of the four affected emitters cannot be reached from an
// executable build at all: `emitExportedIndexerGetters` / `-Setters` are gated
// on `!isMainModule_` (LlvmBackend_impl.hpp — the #2414 gate, kept for the
// latent-codegen-layout reason recorded there), so a public `Object`-typed
// INDEXER only reaches `ensureIndexerGetterFn`'s exported face when the module
// is compiled as a LIBRARY (`bezelc -c`). Both faces are pinned here.
//
// The assertion is that the DEPENDENCY OBJECT BUILDS. Pre-fix
// `Driver::compileObject` failed with
//
//     Function return type does not match operand type of return inst!
//       ret i32 %n
//      ptr
//
// because `setupBoxDescriptors()` had not run yet when the exported accessor's
// body was lowered, so `boxForTarget` was inert and the raw scalar reached the
// `ptr` return. Nothing about the IMPORTER is asserted: a box minted in one
// artifact still carries that artifact's INTERNAL-linkage box vtable, so the
// consumer's `is int` on it answers false — #2301/#2797, a distinct
// cross-artifact box-IDENTITY defect that predates and is untouched by this
// fix (verified: the same false answer is produced for an `Object`-returning
// METHOD, whose body has always been lowered after `setupBoxDescriptors`).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile2864 {
    std::string path;
    explicit TempFile2864(std::string p) : path(std::move(p)) {}
    ~TempFile2864() { std::remove(path.c_str()); }
};

// true iff `libSrc` compiles to a dependency object (`bezelc -c`, the
// isMainModule_ == false path).
bool compilesAsLibrary2864(const std::string& libSrc, const char* stemTag) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = std::string("bz2864_") + stemTag + "_" +
                       unique_stem("");
    TempFile2864 src((dir / (stem + ".bzl")).string());
    TempFile2864 obj((dir / (stem + ".o")).string());
    { std::ofstream out(src.path); out << libSrc; }
    bezel::Driver driver;
    if (!driver.compileObject(src.path, obj.path).success) return false;
    return std::ifstream(obj.path).good();
}

}  // namespace

// ===========================================================================
// T1 — a public `Object`-typed PROPERTY in a library build. // per §8.6, §23.2
// ===========================================================================
BEZEL_TEST(xmod_object_accessor_boxing_2864,
           object_property_library_builds_spec_8_6) {
    BEZEL_EXPECT_TRUE(compilesAsLibrary2864(
        "module Lib2864T1; "
        "public class Bag2864T1 "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public property Q: Object => _n; "
        "end Bag2864T1;",
        "t1"));   // pre-fix: false — IR verification rejected the object
}

// ===========================================================================
// T2 — the INDEXER face, reachable ONLY from a library build. // per §23.4
// ===========================================================================
BEZEL_TEST(xmod_object_accessor_boxing_2864,
           object_indexer_library_builds_spec_23_4) {
    BEZEL_EXPECT_TRUE(compilesAsLibrary2864(
        "module Lib2864T2; "
        "public class Bag2864T2 "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public property [i: int]: Object => _n + i; "
        "end Bag2864T2;",
        "t2"));   // pre-fix: false
}

// ===========================================================================
// T3 — a boxable RECORD and ENUM through the same two faces, so the reorder is
// pinned for the descriptor family #2796 widened, not just for primitives.
// // per §8.6, §9, §10
// ===========================================================================
BEZEL_TEST(xmod_object_accessor_boxing_2864,
           object_accessor_record_and_enum_library_builds_spec_8_6) {
    BEZEL_EXPECT_TRUE(compilesAsLibrary2864(
        "module Lib2864T3; "
        "public record Pt2864T3(X: int, Y: int); "
        "public enum Color2864T3 Red, Green; end Color2864T3; "
        "public class Bag2864T3 "
        "  public property R: Object => new Pt2864T3(1, 2); "
        "  public property E: Object => Color2864T3.Green; "
        "  public property [i: int]: Object => new Pt2864T3(i, i); "
        "end Bag2864T3;",
        "t3"));   // pre-fix: false
}

// ===========================================================================
// T4 — OVER-CORRECTION GUARD, green before and after: a library whose public
// accessors are all NON-Object never depended on the box machinery and must
// still build. // per §23.2, §7
// ===========================================================================
BEZEL_TEST(xmod_object_accessor_boxing_2864,
           non_object_accessor_library_still_builds_spec_23_2) {
    BEZEL_EXPECT_TRUE(compilesAsLibrary2864(
        "module Lib2864T4; "
        "public class Bag2864T4 "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public property N: int => _n * 3; "
        "  public property S: string => \"k\"; "
        "  public property [i: int]: int => _n + i; "
        "end Bag2864T4;",
        "t4"));
}

}  // namespace bezel::module
