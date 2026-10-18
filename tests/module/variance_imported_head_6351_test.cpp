// fix #6351 (§12.9, v0.338): the composed variance-position check composes
// through a generic head it can resolve, and SKIPS one it cannot.
//
// #6348 added `Resolver::checkVariancePositions` ->
// `composedParamPosition` / `namedHeadArgVariance`
// (src/sema/TypeResolution_Checks.cpp). `namedHeadArgVariance` resolves a
// head's own variance from exactly two sources: the CURRENT module's declared
// types (`ctx_.module()->children`, collected into `localTypes` by the pass),
// and the compiler built-ins via `isBuiltinTypeName`. Everything else returned
// `VariancePos::Unknown`, and `composedVarianceViolation` reports no violation
// for Unknown.
//
// An interface IMPORTED from another module is neither, so `out T` with
// `method Get(): IThing<T>` — where Lib exports `public interface IThing<in T>`
// — composed to Unknown and was SILENTLY ACCEPTED. §12.9's rule is
// unconditional: "Position is composed across nested type constructors. A type
// parameter inside another generic type inherits position from that wrapper's
// variance … The compiler enforces position constraints." A head's home module
// is not part of the rule, so this is the same bug #6348 closed for a local
// head, reached by a different route.
//
// The rows are the cross-module mirror of #6348's local rows, so the CONTRAST
// is the point: the same shape with a LOCAL head is already rejected, the same
// shape with an IMPORTED head was not. Both spellings of an imported head are
// covered (`import Lib;` -> `IThing`, and `import Lib as L;` -> `L.IThing`;
// §2.2 — an alias is a qualifier, not a rename).
//
// Every row is two-module (real Driver::compileObject + compileFileForTest
// with a Dependency). The library must build in every row — the defect is in
// the IMPORTER's declaration, not in the export.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

void write6351(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct Result6351 {
    bool libBuilt = false;
    bool appCompiled = false;
    std::string err;
};

Result6351 build6351(const std::string& libSrc, const std::string& appSrc) {
    auto dir = fs::temp_directory_path();
    const std::string id = unique_stem("bz6351_");
    const std::string libF = (dir / (id + "_lib.bzl")).string();
    const std::string libO = (dir / (id + "_lib.o")).string();
    const std::string appF = (dir / (id + "_app.bzl")).string();
    const std::string exeF = (dir / id).string();
    write6351(libF, libSrc);
    write6351(appF, appSrc);
    Result6351 r;
    bezel::Driver driver;
    r.libBuilt = driver.compileObject(libF, libO).success &&
                 std::ifstream(libO).good();
    if (!r.libBuilt) return r;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libO;
    d.objectPath = libO;
    deps.push_back(d);

    const std::string errPath = (dir / (id + "_err.txt")).string();
    std::fflush(stderr);
    std::cerr.flush();
    const int saved = ::dup(STDERR_FILENO);
    const int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    r.appCompiled =
        bezel::test::compileFileForTest(driver, appF, exeF, deps).success;
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    {
        std::ifstream in(errPath);
        r.err.assign((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
        std::remove(errPath.c_str());
    }
    std::remove(exeF.c_str());
    std::remove(libF.c_str());
    std::remove(libO.c_str());
    std::remove(appF.c_str());
    return r;
}

// REJECT: the declared marker cannot be honoured at the composed position,
// and the diagnostic must be the §12.9 one — naming the parameter, the slot,
// the composed-through-a-nested-type-constructor clause and the method.
// `marker` is the declared marker the diagnostic must quote ("out" or "in").
void expectRejected(const std::string& libSrc, const std::string& appSrc,
                    const std::string& marker) {
    auto r = build6351(libSrc, appSrc);
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_FALSE(r.appCompiled);
    const std::string needle =
        marker == "out" ? "type parameter `T` is covariant (`out`)"
                        : "type parameter `T` is contravariant (`in`)";
    BEZEL_EXPECT_TRUE(r.err.find(needle) != std::string::npos);
    BEZEL_EXPECT_TRUE(
        r.err.find("composed through a nested type constructor") !=
        std::string::npos);
    BEZEL_EXPECT_TRUE(r.err.find("(§12.9)") != std::string::npos);
}

// ACCEPT: the composition through the imported head is sound, so the importer
// must compile clean. Guards against a fix that rejects the sound shapes too.
void expectAccepted(const std::string& libSrc, const std::string& appSrc) {
    auto r = build6351(libSrc, appSrc);
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_TRUE(r.appCompiled);
}

// A library exporting one head, spelled the way each app names it.
std::string libWith(const std::string& decl) {
    return "module Lib6351; " + decl;
}

std::string app(const std::string& importLine, const std::string& decls) {
    return "module App6351; import System; " + importLine + decls +
           "public class Program "
           "  public static method Main(): int "
           "    Console.WriteLine(\"x\"); return 0; "
           "  end Main; "
           "end Program; ";
}

}  // namespace

// --- an imported CONTRAVARIANT head under an `out T` ----------------------
// §12.9: "A type parameter inside another generic type inherits position from
// that wrapper's variance." `IThing<T>` in a RETURN slot sits covariantly; the
// wrapper's own `in T` flips it to CONTRAVARIANT, which `out T` cannot honour.
// The identical shape with a LOCAL head is rejected by
// `variance_position_composed_6348.out_t_under_local_contravariant_head_rejected`
// — this row is the cross-module mirror of that one, and is the defect.

BEZEL_TEST(variance_imported_head_6351,
           out_t_under_imported_contravariant_head_rejected) {
    expectRejected(
        libWith("public interface IThing<in T> method Put(x: T); end IThing; "),
        app("import Lib6351; ",
            "public interface I<out T> method Get(): IThing<T>; end I; "),
        "out");
}

// The same, spelled through an import ALIAS: §2.2 — an alias is a qualifier,
// not a rename, so `L.IThing` and `IThing` are one type and must compose alike.

BEZEL_TEST(variance_imported_head_6351,
           out_t_under_imported_contravariant_head_alias_rejected) {
    expectRejected(
        libWith("public interface IThing<in T> method Put(x: T); end IThing; "),
        app("import Lib6351 as L; ",
            "public interface I<out T> method Get(): L.IThing<T>; end I; "),
        "out");
}

// The mirror of the row above with the marker flipped: `in T` under a
// covariant head reached through a RETURN slot composes to covariant, which
// `in T` cannot honour either.

BEZEL_TEST(variance_imported_head_6351,
           in_t_under_imported_covariant_head_return_rejected) {
    expectRejected(
        libWith("public interface IThing<out T> method Get(): T; end IThing; "),
        app("import Lib6351; ",
            "public interface I<in T> method Get(): IThing<T>; end I; "),
        "in");
}

// The sound mirror of that row, and the reason the row above had to move the
// occurrence to a RETURN slot: `in T` under an imported COVARIANT head in an
// INPUT slot is contravariant-of-nothing — contravariant slot, covariant
// wrapper, contravariant result — which is exactly what `in T` promises. It
// must KEEP compiling, which it can only be judged as if the imported head
// were resolved. (Verified against the LOCAL twin, which also compiles.)

BEZEL_TEST(variance_imported_head_6351,
           in_t_under_imported_covariant_head_input_accepted) {
    expectAccepted(
        libWith("public interface IThing<out T> method Get(): T; end IThing; "),
        app("import Lib6351; ",
            "public interface I<in T> method Put(x: IThing<T>); end I; "));
}

// --- an imported CLASS head ----------------------------------------------
// §12.9: "Classes, records, and concrete generic types are always invariant."
// An imported generic class is no different, so `out T` under `Box<T>` in an
// input slot is invariant and rejected — the cross-module mirror of
// `variance_position_composed_6348.out_t_under_array_in_parameter_rejected`.

BEZEL_TEST(variance_imported_head_6351, out_t_under_imported_class_head_rejected) {
    expectRejected(
        libWith("public class Box<T> public constructor() end constructor; "
                "end Box; "),
        app("import Lib6351; ",
            "public interface I<out T> method Put(xs: Box<T>); end I; "),
        "out");
}

// --- the sound compositions must KEEP compiling (no over-rejection) -------
// This is the issue's own worked shape: an imported COVARIANT head under an
// `out T` in a return slot is contravariant-of-nothing — covariant slot,
// covariant wrapper, covariant result. It is legal and must stay legal, which
// it can only be judged as if the head were actually resolved.

BEZEL_TEST(variance_imported_head_6351, out_t_under_imported_covariant_head_accepted) {
    expectAccepted(
        libWith("public interface IThing<out T> method Get(): T; end IThing; "),
        app("import Lib6351; ",
            "public interface I<out T> method Get(): IThing<T>; end I; "));
}

// Contravariant-of-contravariant: `in T` under an imported `in T` head sitting
// in an input slot. §12.9's composition rule makes this covariant — which is
// the ONE marker that cannot honour it, so it must be rejected rather than
// skipped. (Paired with the `out T` row above, this covers both markers
// through an imported contravariant head.)

BEZEL_TEST(variance_imported_head_6351, in_t_under_imported_contravariant_head_rejected) {
    expectRejected(
        libWith("public interface IThing<in T> method Put(x: T); end IThing; "),
        app("import Lib6351; ",
            "public interface I<in T> method Put(x: IThing<T>); end I; "),
        "in");
}

// --- an IMPORTED base-list head must still DEFER (#6355 is untouched) ------
//
// #6348 narrowed the base-list leg so an INVARIANT base yields `Unknown`/
// skip rather than a rejection, because §12.9 is ambiguous about whether a
// base list participates in the position check at all — an OPEN spec-question
// (#6355), unresolved. `interface D<out T>: IInv<T>` with an invariant `IInv`
// therefore COMPILES today.
//
// #6351 makes an imported head resolvable, so this row is the guard that the
// deferral still holds once the head comes from another module: resolving it
// must not turn the deferred shape into a rejection. It is a MEMBER-site fact
// that #6351 leaves alone either way, and this row fails loudly if a future
// change routes a base-list head down the member path.
//
// It is pinned in gtest only — no lit row and no conformance case asserts the
// deferred shape, so there is no `docs/`-derived oracle encoding a reading.

BEZEL_TEST(variance_imported_head_6351,
           imported_invariant_base_still_deferred_spec_12_9) {
    expectAccepted(
        libWith("public interface IInv<T> method Put(x: T); end IInv; "),
        app("import Lib6351; ",
            "public interface D<out T>: IInv<T> end D; "));
}

// The sound mirror of that row: `out T` under an imported `in T` head in an
// INPUT slot is contravariant-of-contravariant = covariant, which is exactly
// what `out T` promises.

BEZEL_TEST(variance_imported_head_6351, out_t_under_imported_contravariant_head_input_accepted) {
    expectAccepted(
        libWith("public interface IThing<in T> method Put(x: T); end IThing; "),
        app("import Lib6351; ",
            "public interface I<out T> method Put(x: IThing<T>); end I; "));
}

// --- the pass still SKIPS what it genuinely cannot resolve ----------------
// A head from a module that was never imported is unresolvable by
// construction, and the pass must keep yielding Unknown there rather than
// guessing. If a fix "resolved" it by simple name across all loaded modules,
// this row would start rejecting.

BEZEL_TEST(variance_imported_head_6351, unresolved_head_still_skipped) {
    // Lib6352 is built and registered, but App6352 never imports it — so
    // `IThing` is not in scope at all and the declaration is a name-resolution
    // failure, not a variance one. Assert the variance diagnostic is ABSENT,
    // proving the check contributed nothing.
    const std::string libSrc =
        "module Lib6352; public interface IThing<in T> "
        "  method Put(x: T); end IThing; ";
    auto dir = fs::temp_directory_path();
    const std::string id = unique_stem("bz6351_skip_");
    const std::string libF = (dir / (id + "_lib.bzl")).string();
    const std::string libO = (dir / (id + "_lib.o")).string();
    const std::string appF = (dir / (id + "_app.bzl")).string();
    const std::string exeF = (dir / id).string();
    write6351(libF, libSrc);
    // Built and linked in, so the symbol exists machine-wide, but the module
    // is never named in an `import` — the spelling is unresolved by §2.2.
    write6351(appF,
              "module App6352; import System; "
              "public interface I<out T> method Get(): IThing<T>; end I; "
              "public class Program "
              "  public static method Main(): int "
              "    Console.WriteLine(\"x\"); return 0; "
              "  end Main; "
              "end Program; ");
    bezel::Driver driver;
    auto lib = driver.compileObject(libF, libO);
    BEZEL_EXPECT_TRUE(lib.success);
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libO;
    d.objectPath = libO;
    deps.push_back(d);
    const std::string errPath = (dir / (id + "_err.txt")).string();
    std::fflush(stderr);
    std::cerr.flush();
    const int saved = ::dup(STDERR_FILENO);
    const int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    const bool ok =
        bezel::test::compileFileForTest(driver, appF, exeF, deps).success;
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    std::ifstream in(errPath);
    std::string err((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    BEZEL_EXPECT_FALSE(ok);
    BEZEL_EXPECT_TRUE(err.find("(§12.9)") == std::string::npos);
    std::remove(errPath.c_str());
    std::remove(exeF.c_str());
    std::remove(libF.c_str());
    std::remove(libO.c_str());
    std::remove(appF.c_str());
}
// --- P9: a LOCAL §11.6 nested head must SHADOW an imported same-named one ---
//
// The regression the non-author review found against the first attempt. The
// local pre-check in `namedHeadArgVariance` scans `ctx_.module()->children` by
// SIMPLE name, so a §11.6 NESTED local head (`Outer.Inner`) is invisible to
// it; `resolveImportedHead`'s qualified branch then matched the imported
// `Outer.Inner` by `qualifiedTypeName()` and read THAT head's marker.
//
// Shape: the app declares `class Outer { interface Inner<out T> }` locally and
// imports `L9`, which exports `Outer.Inner<in T>`. §2.2 makes the local
// declaration shadow the import, so name resolution binds the LOCAL covariant
// head — and `out T` with `method Get(): Outer.Inner<T>` in a return slot is
// exactly the covariant-of-covariant composition `out T` honours. The program
// must COMPILE.
//
// The base (pre-#6351) accepted it, because the head resolved to nothing and
// the pass skipped. The first attempt REJECTED it, having turned a harmless
// skip into a false rejection read off a type the name resolver never bound.
// This row pins the accept side; `outer_inner_shadowing_imported_head_accepted`.

BEZEL_TEST(variance_imported_head_6351,
           outer_inner_shadowing_imported_head_accepted) {
    expectAccepted(
        libWith("public class Outer "
                "  public interface Inner<in T> "
                "    method Put(x: T); "
                "  end Inner; "
                "end Outer; "),
        app("import Lib6351; ",
            "public class Outer "
            "  public interface Inner<out T> "
            "    method Get(): T; "
            "  end Inner; "
            "end Outer; "
            "public interface I<out T> method Get(): Outer.Inner<T>; end I; "));
}

// --- the third legal spelling: MODULE-qualified, no alias (§2.7) ----------
//
// The reviewer's P3 showed the first attempt closed only TWO of the three legal
// spellings of an imported head: bare `IThing` and alias-qualified `L.IThing`
// both worked, but `Lib6351.IThing<T>` — the §2.7 module-qualified spelling,
// legal with a plain `import Lib6351;` and no alias — still silently accepted.
// Cause: `TypeSymbol::qualifiedTypeName()` is the SIMPLE name for a top-level
// type, so a byte-for-byte match against the full dotted spelling could never
// match `Lib6351.IThing`, only a §11.6 nested path.
//
// This row closes it, and pairs with the P9 row above: the module-qualified
// spelling resolves ONLY because a LOCAL top-level `Outer` short-circuits
// first, so the two together pin both halves of the qualifier test.

BEZEL_TEST(variance_imported_head_6351,
           out_t_under_module_qualified_head_rejected) {
    expectRejected(
        libWith("public interface IThing<in T> method Put(x: T); end IThing; "),
        app("import Lib6351; ",
            "public interface I<out T> method Get(): Lib6351.IThing<T>; end I; "),
        "out");
}

// The sound mirror: `out T` under a module-qualified COVARIANT head in a
// return slot is covariant-of-covariant, so it must keep compiling.

BEZEL_TEST(variance_imported_head_6351,
           out_t_under_module_qualified_covariant_head_accepted) {
    expectAccepted(
        libWith("public interface IThing<out T> method Get(): T; end IThing; "),
        app("import Lib6351; ",
            "public interface I<out T> method Get(): Lib6351.IThing<T>; end I; "));
}

// --- the MIRROR of the P9 defect, in the skipping direction ---------------
//
// The mirror of the P9 shape: P9 is a false RESOLVE (a legal program
// rejected), and this is the other direction — a false SKIP, where a real
// violation would be silently accepted because the head resolved to the WRONG
// (more permissive) type or to none. Shape: the app declares a LOCAL
// `class Outer { interface Inner<in T> }` — a CONTRAVARIANT head, which `out T`
// cannot honour in a return slot — beside an imported `Outer.Inner<out T>`
// that would have been benign. A resolver that shadowed wrongly (reading the
// imported covariant head) would ACCEPT here.
//
// BEHAVIOUR FLIP (fix #6369, §11.6, §12.9): this row was ACCEPTED and is now
// REJECTED with §12.9. It had been pinned as documentation of the direction
// only — the non-author review measured it passing on the base, on both #6351
// commits AND under the guard-disabled mutation, because the pre-existing gap
// (a nested LOCAL head resolved by no arm of the composed walk, filed as P8e
// and then as **#6369**) swallowed the case before the shadowing guard was ever
// consulted. Fix #6369 closes that gap, so this is now the row that pins the
// direction it always described: the LOCAL nested head is the one that answers,
// so its CONTRAVARIANT marker is read and `out T` in a return slot is rejected
// — the imported, benign `Outer.Inner<out T>` beside it is correctly not
// consulted.
//
// The row that pins the SHADOWING GUARD itself is still
// `local_type_shadowing_imported_module_name_skips_variance`, which fails when
// `namesLocalTopLevel` is disabled.

BEZEL_TEST(variance_imported_head_6351,
           local_nested_head_reads_local_marker_not_imported) {
    expectRejected(
        libWith("public class Outer "
                "  public interface Inner<out T> "
                "    method Get(): T; "
                "  end Inner; "
                "end Outer; "),
        app("import Lib6351; ",
            "public class Outer "
            "  public interface Inner<in T> "
            "    method Put(x: T); "
            "  end Inner; "
            "end Outer; "
            "public interface I<out T> method Get(): Outer.Inner<T>; end I; "),
        "out");
}

// --- the local-shadow guard's OWN load-bearing case -----------------------
//
// The `namesLocalTopLevel` guard in `resolveImportedHead` is not exercised by
// the P9 row above: there the qualifier `Outer` is neither an import alias nor
// a module name, so the module lookup misses anyway. It bites when a LOCAL
// top-level type shares its name with an IMPORTED MODULE — the one spelling
// where the module-name test alone would have let the import win.
//
// Measured with the guard disabled (fresh binary, 17:03), this program got a
// SECOND, spurious diagnostic on top of the correct one:
//
//   type `Lib6351` has no nested type named `IThing` (§11.3)          <- correct
//   type parameter `T` is covariant (`out`) but appears in an input
//     (parameter) position ... (§12.9)                              <- SPURIOUS
//
// The name resolver bound the LOCAL class (that is what the §11.3 diagnostic
// says: it looked for a NESTED type inside `Lib6351`), so §12.9 must skip —
// reading the imported module's variance there is a false resolve in exactly
// the shape P9 found. With the guard on, only the §11.3 diagnostic is emitted.
//
// This row pins the ABSENCE of the §12.9 needle on a program that is rejected
// for an unrelated, correct reason.

BEZEL_TEST(variance_imported_head_6351,
           local_type_shadowing_imported_module_name_skips_variance) {
    auto r = build6351(
        libWith("public interface IThing<in T> method Put(x: T); end IThing; "),
        app("import Lib6351; ",
            "public class Lib6351 "
            "  public constructor() end constructor; "
            "end Lib6351; "
            "public interface I<out T> "
            "  method Get(): Lib6351.IThing<T>; "
            "end I; "));
    BEZEL_EXPECT_TRUE(r.libBuilt);
    // Rejected — but for the RIGHT reason (§11.3: the local `Lib6351` is a
    // class with no nested `IThing`), and the variance check contributed
    // nothing to it.
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(r.err.find("has no nested type named `IThing` (§11.3)") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(r.err.find("(§12.9)") == std::string::npos);
}

// --- ARM ORDER: a local alias SHADOWS an imported type of the same name ----
//
// #6354 resolves a LOCAL generic alias between the local-type walk and the
// built-in table; #6351 adds the imported arm after it. The order is §2.2's,
// not a preference: "a local declaration shadows an import", so for a name
// that is BOTH an imported type and a local alias the ALIAS must win — that
// is the type the name resolver binds, and this pass reads the marker off
// whatever the name resolver binds. Reachability of the import arm here is
// the discriminator: `Foo<T>` is spelled BARE, and a bare name is exactly the
// spelling `resolveImportedHead` searches every imported module for.
//
// The verdict is discriminating, which is the point. `Foo` is an imported
// `out T` interface (an ALIAS-resolved reading makes it invariant, because
// `type Foo<T> := Array<T>` names an invariant class), so:
//   import-arm-first  -> covariant  -> ACCEPTED   (wrong: reads a marker off
//                                                 a type the name resolver
//                                                 never bound for this module)
//   alias-arm-first   -> invariant  -> REJECTED   (§12.9)
// The row asserts REJECT, so putting the import arm first makes it fail.

BEZEL_TEST(variance_imported_head_6351,
           local_alias_shadows_imported_type_of_the_same_name_rejected) {
    expectRejected(
        libWith("public interface Foo<out T> method Get(): T; end Foo; "),
        app("import Lib6351; ",
            "type Foo<T> := Array<T>; "
            "public interface I<out T> method Get(): Foo<T>; end I; "),
        "out");
}

// The qualified mirror. `resolveLocalAliasHead` matches only a single-part
// name — `TypeAliasDecl` is a `TopDecl` (A.2) and §27.8 declares aliases at
// module level, so a qualified spelling cannot name one — so `L.Foo<T>` skips
// the alias arm entirely and reads the imported `in T`, which under `out T` is
// CONTRAVARIANT and rejected. Same library, same local alias, opposite
// spelling, and the two must not agree: the alias is not a rename of the
// import (that is §2.2's rule for an IMPORT ALIAS, which
// `out_t_under_imported_contravariant_head_alias_rejected` already pins).

BEZEL_TEST(variance_imported_head_6351,
           qualified_import_spelling_bypasses_the_local_alias_rejected) {
    expectRejected(
        libWith("public interface Foo<in T> method Put(x: T); end Foo; "),
        app("import Lib6351; ",
            "type Foo<T> := Array<T>; "
            "public interface I<out T> method Get(): Lib6351.Foo<T>; end I; "),
        "out");
}

// --- THREE SPELLINGS, ONE ANSWER (direct / alias / imported) --------------
//
// §27.8: "Type aliases — generic or not — see through to the aliased type for
// identity, equality, method dispatch, and overload resolution. They are a
// naming convenience, not a separate type kind." §12.9's composition rule
// names no module boundary and no alias carve-out. So a head spelled three
// ways must compose to the SAME position, or one of the three spellings is a
// hole — which is the whole of #6354 (alias) and #6351 (import) respectively.
// These rows are the cross-check: each spelling is paired with the two it
// must agree with, so a merge that dropped either arm shows up as a
// disagreement rather than as a row quietly passing on the other arm.
//
// `in`-mirror of `out` for the trio. All three REJECT, because every reading
// of `CThing<in T>` is CONTRAVARIANT at a return slot, which `out T` cannot
// honour: the DIRECT local spelling (#6348), the local ALIAS spelling
// (#6354 — the alias body `CThing<T>` composed at `CoVariant` reads `CThing`'s
// own `in`), and the IMPORTED spelling (#6351).

BEZEL_TEST(variance_imported_head_6351, direct_local_spelling_rejected) {
    expectRejected(
        libWith("public interface Unused<T> method Put(x: T); end Unused; "),
        app("import Lib6351; ",
            "public interface CThing<in T> method Put(x: T); end CThing; "
            "public interface I<out T> method Get(): CThing<T>; end I; "),
        "out");
}

BEZEL_TEST(variance_imported_head_6351, local_alias_spelling_agrees_rejected) {
    expectRejected(
        libWith("public interface Unused<T> method Put(x: T); end Unused; "),
        app("import Lib6351; ",
            "public interface CThing<in T> method Put(x: T); end CThing; "
            "type AThing<T> := CThing<T>; "
            "public interface I<out T> method Get(): AThing<T>; end I; "),
        "out");
}

BEZEL_TEST(variance_imported_head_6351, imported_spelling_agrees_rejected) {
    expectRejected(
        libWith("public interface CThing<in T> method Put(x: T); end CThing; "),
        app("import Lib6351; ",
            "public interface I<out T> method Get(): CThing<T>; end I; "),
        "out");
}

// The accept-side trio. A COVARIANT head at an output slot is COVARIANT under
// all three spellings, so all three must COMPILE — the rows that keep the fix
// from being a blanket rejection, and the reason the trio above is a real
// agreement check rather than "everything rejects".

BEZEL_TEST(variance_imported_head_6351, direct_local_covariant_accepted) {
    expectAccepted(
        libWith("public interface Unused<T> method Put(x: T); end Unused; "),
        app("import Lib6351; ",
            "public interface GThing<out T> method Get(): T; end GThing; "
            "public interface I<out T> method Get(): GThing<T>; end I; "));
}

BEZEL_TEST(variance_imported_head_6351, local_alias_covariant_agrees_accepted) {
    expectAccepted(
        libWith("public interface Unused<T> method Put(x: T); end Unused; "),
        app("import Lib6351; ",
            "public interface GThing<out T> method Get(): T; end GThing; "
            "type AGThing<T> := GThing<T>; "
            "public interface I<out T> method Get(): AGThing<T>; end I; "));
}

BEZEL_TEST(variance_imported_head_6351, imported_covariant_agrees_accepted) {
    expectAccepted(
        libWith("public interface GThing<out T> method Get(): T; end GThing; "),
        app("import Lib6351; ",
            "public interface I<out T> method Get(): GThing<T>; end I; "));
}

}  // namespace bezel::module
