// fix #3615 (§9.1 / §9.2 / §19.3 / §2.8.2 / §2.8.3 rule 4, v0.286): codegen's
// record-storage type lookup answered NULL for an imported POSITIONAL field and
// for every imported auto-property, because it was a second, drifted spelling of
// `sema::storageComponentType` — the ONE both-sources accessor (sema/Symbol.hpp
// :926, body SymbolCollection.cpp:1051).
//
// THE DRIFT. `Emitter::recordFieldType` (LlvmBackend_String.cpp) gated its
// imported fallback on `if (!fs->posParam && !fs->decl)`. Fix #2874's own comment
// directly above that gate NAMES the case the gate excludes, verbatim: "the
// loader synthesizes a null-type posParam only for §9.1 positional-vs-body
// discrimination, not the type itself; the type itself is never recoverable from
// posParam for a #2516 imported positional field". ModuleLoader.cpp's
// `if (mm.isPositional)` arm does exactly that — points `posParam` at a synthetic
// `ast::Param` with a null `type` — so `!fs->posParam` is FALSE for precisely the
// field kind #2874's fallback was written for, and the fallback never ran.
// `Emitter::recordMemberType`'s §9.2 property arm read `dynamic_cast<const
// ast::PropertyDecl*>(m->decl)` with no imported fallback at all; `m->decl` is
// null for every imported member.
//
// `sema::storageComponentType` has the same two AST steps and then reaches
// `ctx.importedMemberType(m)` UNCONDITIONALLY, on both the field and the property
// arm — which is what ModuleLoader's own #2516 comment says the design relies on.
// The fix is delegation, not a seventh copy of the lookup.
//
// WHAT IT COST (both measured against a CHARACTER-IDENTICAL single-module twin;
// both live on `main` @ 907ce0f).  Both are §19.3 record-destructuring
// sub-patterns, whose lowering (LlvmBackend_Exprs2.cpp) asks `recordMemberType`
// for the field's declared type to decide how to compare that slot:
//
//   T1 SILENT WRONG ANSWER. A `string` sub-pattern. With a null field type the
//      lowering leaves `subIsStr` false and compares the slot by POINTER instead
//      of by content (that comment is #615's, in the code). `Tag("abc", _)` against
//      a `Tag` whose `Name` was BUILT at run time ("ab" + "c") answered 3 (the `_`
//      arm) cross-module and 1 locally. Exit 0, wrong value.
//
//   T2 HARD REJECT. An enum-constant sub-pattern. With a null field type the
//      lowering cannot resolve `subEnumTy`, and the backend rejects:
//      "the `case`/`when` pattern `Blue` is not supported by the backend: it did
//      not resolve to a type, enum constant, or named constant to match against
//      (§19.3)". The identical single-module program compiles and matches.
//
// §2.8.3 rule 4 is explicit that "[§19] patterns over values of the
// instantiation match identically regardless of which artifact produced the value
// or performs the test", so both are rule-4 violations in as many words.
//
// THE §6b EXCLUSION DIFF — what the unified lookup ADMITS that the two old copies
// EXCLUDED. `SemaContext::importedMemberTypes_` is written by exactly one caller,
// `ModuleLoader.cpp:4331` (`ctx.setImportedMemberType`), so `importedMemberType`
// is null for every LOCAL symbol and the widening is confined to imports. Within
// imports it is exactly two sets:
//   (a) a #2516 imported POSITIONAL field (non-null synthetic `posParam`) — the
//       old field gate said no, `storageComponentType` says yes;
//   (b) an imported PROPERTY (auto-property storage) — the old property arm had
//       no fallback whatever.
// Nothing else changes: for a LOCAL field or property both spellings take the
// same AST step and return the same pointer (verified by an instrumented build
// that computed BOTH and printed every disagreement — over the probes above, the
// only disagreements were `pos=1 decl=0` imported fields and decl-null imported
// properties, and the local twins printed `old == new` on every member).
// Dispatch is likewise equivalent on the local side: SymbolCollection.cpp:236
// makes a `PropertySymbol` for every `ast::PropertyDecl` and nothing else is ever
// given one, so "decl is a PropertyDecl" and "symbol is a PropertySymbol with an
// AST decl" name the same set.
//
// FALSE-REJECT COST: 0. Every in-tree `.bzl` (2863 files) was compiled with a
// PARENT-`main` `bezelc` and with the fixed one and the exit status AND full
// diagnostic text compared; see the commit body for the count.
//
// ⚠ Not a `runAot` test: the observable is cross-module, and `runAot` compiles
// with `deps={}`. It drives the same real Driver path (compileObject → compileFile
// → link → native exec), one process per program.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {
namespace fs = std::filesystem;

const std::filesystem::path& wtDir3615() { return wtScratchDir("wt-3615"); }

struct TempFile3615 {
    std::string path;
    explicit TempFile3615(std::string p) : path(std::move(p)) {}
    ~TempFile3615() { std::remove(path.c_str()); }
};

void write3615(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile lib -> object, compile main importing lib -> executable, run.
// Returns the exit code, or -1 when either compile or the link fails. A -1 is a
// MEANINGFUL outcome here: T2's pre-fix state is a backend REJECT, so the test
// must be able to see "did not build" as distinct from "built and answered".
int runXmod3615(const std::string& libSrc, const std::string& mainSrc) {
    auto dir = wtDir3615();
    std::string stem = unique_stem("bezel_xmod3615");
    TempFile3615 lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile3615 lObj((dir / (stem + "_lib.o")).string());
    TempFile3615 mSrc((dir / (stem + "_main.bzl")).string());
    TempFile3615 mExe((dir / (stem + "_main")).string());

    write3615(lSrc.path, libSrc);
    write3615(mSrc.path, mainSrc);

    bezel::Driver driver;
    if (!driver.compileObject(lSrc.path, lObj.path).success) return -1;

    bezel::Dependency dep;
    dep.metadataPath = lObj.path;
    dep.objectPath   = lObj.path;
    std::vector<bezel::Dependency> deps{dep};

    if (!driver.compileFile(mSrc.path, mExe.path, deps).success) return -1;

    int status = std::system(mExe.path.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

// The single-module CONTROL: the SAME declarations and the SAME body in one
// file. Every cross-module expectation below is paired with one of these, so a
// wrong expected value shows up as the control failing too.
int runLocal3615(const std::string& src) {
    auto dir = wtDir3615();
    std::string stem = unique_stem("bezel_loc3615");
    TempFile3615 mSrc((dir / (stem + ".bzl")).string());
    TempFile3615 mExe((dir / stem).string());
    write3615(mSrc.path, src);

    bezel::Driver driver;
    if (!driver.compileFile(mSrc.path, mExe.path, {}).success) return -1;
    int status = std::system(mExe.path.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

// The dependency's declarations. `Tag` is a §9.1 POSITIONAL record: its two
// FieldSymbols are the #2516 shape (synthetic non-null `posParam`, null `type`)
// on the importer's side, which is exactly what the old gate excluded.
const char* kDecls3615 =
    "public enum Color  Red, Green, Blue; end Color; "
    "public record Tag(Name: string, C: Color); ";

std::string lib3615() {
    return std::string("module Lib; ") + kDecls3615;
}

std::string app3615(const std::string& body) {
    return "module App; import Lib; class C " + body + " end C; ";
}

std::string local3615(const std::string& body) {
    return std::string("module App; ") + kDecls3615 + " class C " + body +
           " end C; ";
}

// §19.3 string sub-pattern. `Name` is built at RUN TIME ("ab" + "c") on purpose:
// a literal would be interned and the pre-fix POINTER comparison would answer
// correctly by accident, and the test would pass over the live defect.
const char* kStrBody3615 =
    "  public static method Cls(t: Tag): int "
    "    return (case t of Tag(\"abc\", _) => 1, _ => 3); "
    "  end Cls; "
    "  public static method Main(): int "
    "    var a := \"ab\"; "
    "    var b := \"c\"; "
    "    return Cls(new Tag(a + b, Color.Red)); "
    "  end Main; ";

// The OVER-REJECTION control for the same arm: a genuinely non-matching value
// must still take the `_` arm. A fix that made every string sub-pattern match
// would pass kStrBody3615 and fail here.
const char* kStrMissBody3615 =
    "  public static method Cls(t: Tag): int "
    "    return (case t of Tag(\"abc\", _) => 1, _ => 3); "
    "  end Cls; "
    "  public static method Main(): int "
    "    var a := \"zz\"; "
    "    var b := \"z\"; "
    "    return Cls(new Tag(a + b, Color.Red)); "
    "  end Main; ";

// §19.3 enum-constant sub-pattern.
const char* kEnumBody3615 =
    "  public static method Cls(t: Tag): int "
    "    return (case t of Tag(_, Color.Blue) => 2, _ => 3); "
    "  end Cls; "
    "  public static method Main(): int "
    "    return Cls(new Tag(\"zzz\", Color.Blue)); "
    "  end Main; ";

// The enum arm's OVER-REJECTION control: a different constant must NOT match.
// This is the arm that mattered most — with a null field type the lowering
// resolved no `subEnumTy`, and #615's own comment records that the historical
// failure mode of that state was "NamedPattern fell through -> AND skipped ->
// always matched". A fix that restored the enum arm by making it vacuously true
// would pass kEnumBody3615 and fail here.
const char* kEnumMissBody3615 =
    "  public static method Cls(t: Tag): int "
    "    return (case t of Tag(_, Color.Blue) => 2, _ => 3); "
    "  end Cls; "
    "  public static method Main(): int "
    "    return Cls(new Tag(\"zzz\", Color.Red)); "
    "  end Main; ";

}  // namespace

// #3615 / §19.3 + §2.8.3 rule 4 — T1, THE SILENT WRONG ANSWER. Pre-fix: 3.
BEZEL_TEST(xmod_record_member_type_3615, imported_positional_string_subpattern_matches) {
    BEZEL_EXPECT_EQ(runXmod3615(lib3615(), app3615(kStrBody3615)), 1);
}

// #3615 — T1's single-module control. Correct on BOTH sides of the fix; it is
// here so a wrong expected value cannot be mistaken for the defect.
BEZEL_TEST(xmod_record_member_type_3615, local_positional_string_subpattern_matches) {
    BEZEL_EXPECT_EQ(runLocal3615(local3615(kStrBody3615)), 1);
}

// #3615 — T1's over-rejection control: the non-matching value still takes `_`.
BEZEL_TEST(xmod_record_member_type_3615, imported_positional_string_subpattern_non_match) {
    BEZEL_EXPECT_EQ(runXmod3615(lib3615(), app3615(kStrMissBody3615)), 3);
}

BEZEL_TEST(xmod_record_member_type_3615, local_positional_string_subpattern_non_match) {
    BEZEL_EXPECT_EQ(runLocal3615(local3615(kStrMissBody3615)), 3);
}

// #3615 / §19.3 — T2, THE HARD REJECT. Pre-fix the program did not build at all
// ("the `case`/`when` pattern `Blue` is not supported by the backend"), so
// runXmod3615 returned its -1 build sentinel; the single-module twin built and
// answered 2.
BEZEL_TEST(xmod_record_member_type_3615, imported_positional_enum_subpattern_matches) {
    BEZEL_EXPECT_EQ(runXmod3615(lib3615(), app3615(kEnumBody3615)), 2);
}

BEZEL_TEST(xmod_record_member_type_3615, local_positional_enum_subpattern_matches) {
    BEZEL_EXPECT_EQ(runLocal3615(local3615(kEnumBody3615)), 2);
}

// #3615 — T2's over-rejection control: `Color.Red` must NOT take the `Blue` arm.
BEZEL_TEST(xmod_record_member_type_3615, imported_positional_enum_subpattern_non_match) {
    BEZEL_EXPECT_EQ(runXmod3615(lib3615(), app3615(kEnumMissBody3615)), 3);
}

BEZEL_TEST(xmod_record_member_type_3615, local_positional_enum_subpattern_non_match) {
    BEZEL_EXPECT_EQ(runLocal3615(local3615(kEnumMissBody3615)), 3);
}

}  // namespace bezel::module
