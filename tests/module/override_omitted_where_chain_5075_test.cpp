// Spec-derived regression test for issue #5075 — a 3-OR-MORE level override
// chain, where the immediate base ALSO omitted its own `where` clause,
// reproduces the #5039 mechanism one level deeper: the SECOND (and any
// further) consecutive omission still loses the inherited constraint set.
//
// spec §14.2 "Constraints on overrides" (v0.307): "An `override` of a generic
// method ... **inherits the overridden ... declaration's constraints**.
// Omitting the `where` clauses is the recommended spelling and means exactly
// the inherited set" — stated with no depth bound, so a 3-level (or deeper)
// chain is exactly as covered by the rule as a 2-level one.
//
// ── ROOT CAUSE (MEASURED on 858d9fa4, i.e. WITH #5039's fix applied) ────────
//
// #5039's fix to `Resolver::checkOverrideSignatureMatch`
// (src/sema/TypeResolution_Checks.cpp) materialized the inherited constraint
// set onto an omitted-`where` override's OWN `GenericParamSymbol`, but read
// ONLY the immediate base's own AST `where` clause (`bDecl->genericConstraints`
// / the imported `ImportedGenericConstraint` side table). When the immediate
// base ALSO omitted its clause (chaining the recommended spelling down two
// consecutive levels), that AST carries nothing, and the #5039 walk stopped
// there — leaving a THIRD-level override's constraints empty again, exactly
// reproducing the #5039 wrong-dispatch mechanism (an empty constraint list
// makes codegen's `genericParamSoundAtI32ForPlaceholder`,
// LlvmBackend_Exprs.cpp, treat the placeholder body's `T` as unsound for the
// i32 cross-module-linkage fast path, so `a.Equals(b)` silently takes the
// wrong branch).
//
// ── THE FIX ──────────────────────────────────────────────────────────────
//
// `materializeConstraintUpChain` (src/sema/TypeResolution_Checks.cpp, added
// by #5075) climbs the same-module ancestor chain by name+arity, one class
// at a time, until it finds a declaration that actually WRITES a `where`
// clause (or an imported ancestor's side table) — not just the immediate
// base. It reads the AST directly rather than the immediate base's
// already-materialized `GenericParamSymbol::constraints`, because
// `checkHidingAndOverride` visits classes in DECLARATION order, and a
// derived class may legally be declared textually BEFORE its own base
// (confirmed empirically: a forward-referenced base class compiles clean),
// so relying on materialization-by-the-time-we-get-there is not reliable.
//
// ── WHAT EACH ROW PINS ───────────────────────────────────────────────────
//
//   T1 ★ THE REPRO (two consecutive omissions, chain depth 3): Base(explicit)
//        <- Mid(omit) <- Leaf(omit), cross-module dispatch through a
//        `Base`-typed reference -> 91 (pre-#5075-fix: 92, matching #5075's
//        own reported repro).
//   T2   same-module twin of T1 — confirms the fix does not depend on the
//        cross-module element (matches #5039's own test's T2 pattern), and
//        that a genuinely unconstrained `T` is not accidentally admitted by
//        an over-eager climb (see #5075 note: same-module dispatch of this
//        exact shape already returned 91 even before #5075, because a
//        same-module virtual call does not go through the i32-placeholder
//        codegen path at all — this row pins that the fix does not disturb
//        that).
//   T3 ★ A THREE-OMISSION CHAIN (chain depth 4): Base(explicit) <-
//        Mid(omit) <- Leaf(omit) <- Grand(omit), cross-module -> 91. Proves
//        the fix climbs an ARBITRARY number of consecutive omissions, not
//        just one extra hop past the immediate base (a hardcoded "look one
//        level further" patch would still fail this row).
//   T4   CONTROL: the middle override (`Mid`) RESTATES its `where` clause
//        explicitly; only the leaf omits. Cross-module -> 91 on both sides
//        of the fix (this is exactly the shape #5039's own fix already
//        handles via `bDecl`'s direct AST clause — it isolates that the
//        defect is specific to a SECOND consecutive omission, per the
//        issue's own "isolating the exact trigger" note).
//   T5   An OVERLOADED generic method in the chain: the base declares both
//        the constrained generic overload and a same-arity non-generic
//        sibling with the same name. Pins that the climb's ancestor search
//        disambiguates by generic arity, not just name+param-count, so it
//        does not silently match the wrong overload when passing through an
//        intermediate class to reach the true declaring ancestor.
//        ⚠ (#5091) T5 does NOT pin that: its sibling is declared AFTER the
//        real overload, so declaration order alone finds the right member,
//        and it survives deleting the generic-arity check (and deleting the
//        parameter-type check, measured for #5091). Kept as a
//        control; T6/T7 are the rows that pin overload disambiguation.
//   T6 ★ (#5091) DECOY-FIRST overload, the reviewer's exact shape:
//        `Base5075F` declares `EqV5075F<T>(a: T, b: string) where T: new()`
//        BEFORE the real `EqV5075F<T>(a: T, b: T) where T: IEquatable<T>`;
//        Mid/Leaf both override the real one omitting `where`. With a
//        shape-only ancestor match (name + param count + generic arity) the
//        climb copies the decoy's `new()` onto Leaf's `T` and the dependency
//        compile reports the `internal:` permissive-acceptance WARNING on
//        Leaf's `a.Equals(b)`. Oracle: zero errors and no `internal:` in
//        the captured diagnostics. No runtime call: two same-arity generic
//        overloads of one name make any call to that name unresolvable today
//        (#5101; see T7).
//   T7 ★ (#5091) DECOY-FIRST overload, callable: the decoy differs from the
//        real overload by a parameter MODIFIER (`ref b: T`) instead, which
//        `overrideParamsMatch` also compares and which keeps the importer's
//        `g.EqV5075G(5, 5)` resolvable -> 91 through a `Base`-typed
//        reference, cross-module.
//
//   No row pins the generic-arity half of the ancestor predicate, and none
//   can: a decoy that differs from the real overload ONLY in generic arity
//   (`M<T, U>(a: T, b: U)` beside `M<T>(a: T, b: T)`) is rejected at its
//   declaration as an indistinguishable overload (§22.6), so no accepted
//   program reaches the climb with one. The check stays as a guard for the
//   positional `gi` read.
//
// Every row except T6 (a compile-diagnostics row) asserts the exact exit
// code (91/92 both fit the 8-bit exit-code range, so no truncation risk —
// CLAUDE.md's "8-bit exit truncation" trap).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile5075 {
    std::string path;
    explicit TempFile5075(std::string p) : path(std::move(p)) {}
    ~TempFile5075() { std::remove(path.c_str()); }
};

void writeSource5075(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Build the dependency (`libSrc`), then compile+link an importer (`mainSrc`)
// against it and RUN it. Returns the exit code, or -2 when the dependency
// toolchain is unavailable (skip the row), -1 on a compile/link/run failure.
// Mirrors override_omitted_where_clause_xmod_5039_test.cpp's
// buildImportAndRun5039 (goes through compileFileForTest/runProgramForTest,
// #4532, so gate.sh --full's generated-code ASan sweep reaches this
// hand-rolled two-module runner too).
int buildImportAndRun5075(const std::string& libSrc,
                          const std::string& mainSrc) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz5075r_");
    TempFile5075 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile5075 libObjF((dir / (id + "_lib.o")).string());
    TempFile5075 mainSrcF((dir / (id + "_main.bzl")).string());
    TempFile5075 mainExeF((dir / id).string());
    writeSource5075(libSrcF.path, libSrc);
    writeSource5075(mainSrcF.path, mainSrc);

    bezel::Driver driver;
    if (!driver.compileObject(libSrcF.path, libObjF.path).success) return -2;
    if (!std::ifstream(libObjF.path).good()) return -2;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjF.path;
    d.objectPath = libObjF.path;
    deps.push_back(d);

    if (!bezel::test::compileFileForTest(driver, mainSrcF.path, mainExeF.path,
                                         deps).success)
        return -1;
    int status = bezel::test::runProgramForTest(mainExeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The one-module twin — no dependency anywhere.
int buildLocalAndRun5075(const std::string& src) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz5075l_");
    TempFile5075 srcF((dir / (id + ".bzl")).string());
    TempFile5075 exeF((dir / id).string());
    writeSource5075(srcF.path, src);
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, srcF.path, exeF.path).success)
        return -1;
    int status = bezel::test::runProgramForTest(exeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// ---------------------------------------------------------------------------
// T1/T2: two consecutive omissions (chain depth 3) — the issue's own repro.
// `Base5075A` declares the clause; `Mid5075A` overrides it OMITTING the
// clause; `Leaf5075A` overrides `Mid5075A` ALSO omitting the clause.
// ---------------------------------------------------------------------------
const char* kChain2Body5075 =
    "public class Base5075A "
    "  public constructor() end constructor; "
    "  public virtual method EqV5075<T>(a: T, b: T): int where T: "
    "IEquatable<T> "
    "    if a.Equals(b) then return 61; end if; "
    "    return 62; "
    "  end EqV5075; "
    "end Base5075A; "
    "public class Mid5075A : Base5075A "
    "  public constructor() end constructor; "
    "  public override method EqV5075<T>(a: T, b: T): int "
    "    if a.Equals(b) then return 81; end if; "
    "    return 82; "
    "  end EqV5075; "
    "end Mid5075A; "
    "public class Leaf5075A : Mid5075A "
    "  public constructor() end constructor; "
    "  public override method EqV5075<T>(a: T, b: T): int "
    "    if a.Equals(b) then return 91; end if; "
    "    return 92; "
    "  end EqV5075; "
    "end Leaf5075A; ";

// ---------------------------------------------------------------------------
// T3: three consecutive omissions (chain depth 4) — generalizes past a
// hardcoded "look one level further" patch. `Base5075B` declares the clause;
// `Mid5075B`, `Leaf5075B`, `Grand5075B` each override the PREVIOUS one,
// every one of them omitting the clause.
// ---------------------------------------------------------------------------
const char* kChain3Body5075 =
    "public class Base5075B "
    "  public constructor() end constructor; "
    "  public virtual method EqV5075<T>(a: T, b: T): int where T: "
    "IEquatable<T> "
    "    if a.Equals(b) then return 61; end if; "
    "    return 62; "
    "  end EqV5075; "
    "end Base5075B; "
    "public class Mid5075B : Base5075B "
    "  public constructor() end constructor; "
    "  public override method EqV5075<T>(a: T, b: T): int "
    "    if a.Equals(b) then return 81; end if; "
    "    return 82; "
    "  end EqV5075; "
    "end Mid5075B; "
    "public class Leaf5075B : Mid5075B "
    "  public constructor() end constructor; "
    "  public override method EqV5075<T>(a: T, b: T): int "
    "    if a.Equals(b) then return 71; end if; "
    "    return 72; "
    "  end EqV5075; "
    "end Leaf5075B; "
    "public class Grand5075B : Leaf5075B "
    "  public constructor() end constructor; "
    "  public override method EqV5075<T>(a: T, b: T): int "
    "    if a.Equals(b) then return 91; end if; "
    "    return 92; "
    "  end EqV5075; "
    "end Grand5075B; ";

// ---------------------------------------------------------------------------
// T4: CONTROL — the middle override (`Mid5075C`) RESTATES its `where`
// clause explicitly; only the leaf (`Leaf5075C`) omits. Already handled by
// #5039 alone (the immediate base's OWN AST clause is non-empty), so this
// is a non-discriminating control.
// ---------------------------------------------------------------------------
const char* kChainRestateBody5075 =
    "public class Base5075C "
    "  public constructor() end constructor; "
    "  public virtual method EqV5075<T>(a: T, b: T): int where T: "
    "IEquatable<T> "
    "    if a.Equals(b) then return 61; end if; "
    "    return 62; "
    "  end EqV5075; "
    "end Base5075C; "
    "public class Mid5075C : Base5075C "
    "  public constructor() end constructor; "
    "  public override method EqV5075<T>(a: T, b: T): int where T: "
    "IEquatable<T> "
    "    if a.Equals(b) then return 81; end if; "
    "    return 82; "
    "  end EqV5075; "
    "end Mid5075C; "
    "public class Leaf5075C : Mid5075C "
    "  public constructor() end constructor; "
    "  public override method EqV5075<T>(a: T, b: T): int "
    "    if a.Equals(b) then return 91; end if; "
    "    return 92; "
    "  end EqV5075; "
    "end Leaf5075C; ";

// ---------------------------------------------------------------------------
// T5: an OVERLOADED generic method in the chain. `Base5075E` declares the
// constrained generic virtual `EqV5075E<T>` (`where T: IEquatable<T>`) AND a
// same-name, same-arity, non-generic **private** sibling
// (`EqV5075E(a: int, b: int)`, zero generic params). The sibling is `private`
// so it is invisible to the external call site (§13 — name lookup filters by
// visibility before overload resolution ever runs) and cannot make
// `g.EqV5075E(5, 5)` ambiguous; it exists purely so the two same-name,
// same-param-count members are both present in `Base5075E`'s member list for
// `materializeConstraintUpChain`'s **internal AST walk** to potentially
// collide on. `Mid5075E`/`Leaf5075E` both override the GENERIC overload,
// omitting `where`. Pins that `materializeConstraintUpChain`'s ancestor
// search (src/sema/TypeResolution_Checks.cpp) disambiguates by generic arity
// as well as name+param-count when it climbs past `Mid5075E` to
// `Base5075E`'s members — name+params.size() alone would risk matching the
// non-generic private sibling (same name, same param count) instead of the
// actual overridden overload, silently missing the real constraint with no
// diagnostic.
// ---------------------------------------------------------------------------
const char* kChainOverloadBody5075 =
    "public class Base5075E "
    "  public constructor() end constructor; "
    "  public virtual method EqV5075E<T>(a: T, b: T): int where T: "
    "IEquatable<T> "
    "    if a.Equals(b) then return 61; end if; "
    "    return 62; "
    "  end EqV5075E; "
    "  private method EqV5075E(a: int, b: int): int "
    "    return 63; "
    "  end EqV5075E; "
    "end Base5075E; "
    "public class Mid5075E : Base5075E "
    "  public constructor() end constructor; "
    "  public override method EqV5075E<T>(a: T, b: T): int "
    "    if a.Equals(b) then return 81; end if; "
    "    return 82; "
    "  end EqV5075E; "
    "end Mid5075E; "
    "public class Leaf5075E : Mid5075E "
    "  public constructor() end constructor; "
    "  public override method EqV5075E<T>(a: T, b: T): int "
    "    if a.Equals(b) then return 91; end if; "
    "    return 92; "
    "  end EqV5075E; "
    "end Leaf5075E; ";

// ---------------------------------------------------------------------------
// T6 (#5091): DECOY-FIRST overload, the reviewer's #5091 repro shape.
// `Base5075F` declares the decoy `EqV5075F<T>(a: T, b: string) where T:
// new()` FIRST — same name, same parameter count (2), same generic arity (1)
// as the real overridden overload declared second, differing only in the
// second parameter's TYPE. `Mid5075F` and `Leaf5075F` both override the REAL
// overload, omitting `where`.
//
// Deliberately no call to `EqV5075F`: with two same-arity generic overloads
// of one name, `g.EqV5075F(5, 5)` is rejected today as "no overload of
// `EqV5075F` is applicable" (argument 2 checked against `string`) — in the
// same module or cross-module, with or without the chain, with the decoy
// first or second, `private` or not. That is a call-resolution defect
// independent of this climb (#5101) and must not be what decides this row.
// ---------------------------------------------------------------------------
const char* kChainDecoyFirstBody5075 =
    "module Dep5075T6; "
    "public class Base5075F "
    "  public constructor() end constructor; "
    "  public virtual method EqV5075F<T>(a: T, b: string): int where T: "
    "new() "
    "    return 63; "
    "  end EqV5075F; "
    "  public virtual method EqV5075F<T>(a: T, b: T): int where T: "
    "IEquatable<T> "
    "    if a.Equals(b) then return 61; end if; "
    "    return 62; "
    "  end EqV5075F; "
    "end Base5075F; "
    "public class Mid5075F : Base5075F "
    "  public constructor() end constructor; "
    "  public override method EqV5075F<T>(a: T, b: T): int "
    "    if a.Equals(b) then return 81; end if; "
    "    return 82; "
    "  end EqV5075F; "
    "end Mid5075F; "
    "public class Leaf5075F : Mid5075F "
    "  public constructor() end constructor; "
    "  public override method EqV5075F<T>(a: T, b: T): int "
    "    if a.Equals(b) then return 91; end if; "
    "    return 92; "
    "  end EqV5075F; "
    "end Leaf5075F; ";

// ---------------------------------------------------------------------------
// T7 (#5091): DECOY-FIRST overload that stays CALLABLE. The decoy
// `EqV5075G<T>(a: T, ref b: T) where T: new()` is declared FIRST and differs
// from the real overload only by the `ref` modifier on its second parameter —
// part of the signature (§22.6) and compared by `overrideParamsMatch`, but
// invisible to a name + param-count + generic-arity match. A `ref` parameter
// takes no plain value argument, so `g.EqV5075G(5, 5)` resolves to the real
// overload and the row can observe Leaf's body at run time.
// ---------------------------------------------------------------------------
const char* kChainDecoyRefFirstBody5075 =
    "public class Base5075G "
    "  public constructor() end constructor; "
    "  public virtual method EqV5075G<T>(a: T, ref b: T): int where T: "
    "new() "
    "    return 63; "
    "  end EqV5075G; "
    "  public virtual method EqV5075G<T>(a: T, b: T): int where T: "
    "IEquatable<T> "
    "    if a.Equals(b) then return 61; end if; "
    "    return 62; "
    "  end EqV5075G; "
    "end Base5075G; "
    "public class Mid5075G : Base5075G "
    "  public constructor() end constructor; "
    "  public override method EqV5075G<T>(a: T, b: T): int "
    "    if a.Equals(b) then return 81; end if; "
    "    return 82; "
    "  end EqV5075G; "
    "end Mid5075G; "
    "public class Leaf5075G : Mid5075G "
    "  public constructor() end constructor; "
    "  public override method EqV5075G<T>(a: T, b: T): int "
    "    if a.Equals(b) then return 91; end if; "
    "    return 92; "
    "  end EqV5075G; "
    "end Leaf5075G; ";

std::string dep5075(const std::string& tag, const std::string& body) {
    return "module Dep5075" + tag + "; " + body;
}

std::string app5075(const std::string& tag, const std::string& baseType,
                    const std::string& leafType) {
    return "module App5075" + tag + "; import Dep5075" + tag + "; " +
           "class C5075" + tag + " public static method Main(): int " +
           "var g: " + baseType + " := new " + leafType + "(); " +
           "return g.EqV5075(5, 5); " +
           " end Main; end C5075" + tag + ";";
}

std::string local5075(const std::string& tag, const std::string& body,
                      const std::string& baseType,
                      const std::string& leafType) {
    return "module Loc5075" + tag + "; " + body +
           "class C5075" + tag + " public static method Main(): int " +
           "var g: " + baseType + " := new " + leafType + "(); " +
           "return g.EqV5075(5, 5); " +
           " end Main; end C5075" + tag + ";";
}

// T5's methods are named `EqV5075E` (distinct from the `EqV5075` used above).
// The call passes int literals and unambiguously reaches the public GENERIC
// overload: the private non-generic sibling of the same name/arity is not a
// visible candidate from this external call site at all (§13) — same
// convention as app5075, parameterized on the member name.
std::string appOverload5075(const std::string& tag, const std::string& baseType,
                            const std::string& leafType) {
    return "module App5075" + tag + "; import Dep5075" + tag + "; " +
           "class C5075" + tag + " public static method Main(): int " +
           "var g: " + baseType + " := new " + leafType + "(); " +
           "return g.EqV5075E(5, 5); " +
           " end Main; end C5075" + tag + ";";
}



// ---------------------------------------------------------------------------
// T8: a 3-MODULE chain that SPANS modules — the imported-ancestor branch of
// `materializeConstraintUpChain`. `Dep3A5075T8` declares `Base5075H` with the
// explicit `where T: IEquatable<T>` clause. `Dep3B5075T8` imports
// `Dep3A5075T8` and declares `Mid5075H : Base5075H` overriding the virtual
// while omitting `where`, then `Leaf5075H : Mid5075H` doing the same. The
// final App imports both deps and dispatches `g.EqV5075H(5, 5)` through a
// `Base5075H`-typed reference into `Leaf5075H`'s override.
//
// This is the only row where the climb crosses an `import` boundary on the
// way up: `materializeConstraintUpChain` walks Leaf -> Mid (same module as
// Leaf, both in Dep3B) -> Base (in Dep3A, IMPORTED into Dep3B). The
// imported-ancestor branch reads Base's `importedGenericConstraints` side
// table rather than its raw AST `where` clause (Dep3B cannot see Dep3A's
// source — only the metadata object). Without #5091's fix-forward, the
// climb stops at Mid's empty AST clause (the imported side table is never
// consulted), Leaf's `T` is left with an empty constraint set, and
// codegen's i32-placeholder fast path silently takes the wrong branch —
// `a.Equals(b)` evaluates to false, exit 92 (matching #5075's reported
// repro).
// ---------------------------------------------------------------------------
const char* kChain3ModBaseBody5075 =
    "public class Base5075H "
    "  public constructor() end constructor; "
    "  public virtual method EqV5075H<T>(a: T, b: T): int where T: "
    "IEquatable<T> "
    "    if a.Equals(b) then return 61; end if; "
    "    return 62; "
    "  end EqV5075H; "
    "end Base5075H; ";

const char* kChain3ModMidLeafBody5075 =
    "public class Mid5075H : Base5075H "
    "  public constructor() end constructor; "
    "  public override method EqV5075H<T>(a: T, b: T): int "
    "    if a.Equals(b) then return 81; end if; "
    "    return 82; "
    "  end EqV5075H; "
    "end Mid5075H; "
    "public class Leaf5075H : Mid5075H "
    "  public constructor() end constructor; "
    "  public override method EqV5075H<T>(a: T, b: T): int "
    "    if a.Equals(b) then return 91; end if; "
    "    return 92; "
    "  end EqV5075H; "
    "end Leaf5075H; ";

// Compile `baseSrc` (the Base-only dep), then `midLeafSrc` (the Mid+Leaf
// dep, which itself imports the Base dep), then link+run `appSrc` against
// both .o files. Returns the exit code, or -2 when the dependency toolchain
// is unavailable (skip), -1 on compile/link/run failure. Mirrors the
// `buildThird5009` helper from xmod_constraint_dispatch_generic_method_5009.
int buildThreeModuleAndRun5075(const std::string& baseSrc,
                               const std::string& midLeafSrc,
                               const std::string& appSrc) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz5075t8_");
    TempFile5075 baseSrcF((dir / (id + "_a.bzl")).string());
    TempFile5075 baseObjF((dir / (id + "_a.o")).string());
    TempFile5075 midSrcF((dir / (id + "_b.bzl")).string());
    TempFile5075 midObjF((dir / (id + "_b.o")).string());
    TempFile5075 appSrcF((dir / (id + "_app.bzl")).string());
    TempFile5075 appExeF((dir / id).string());
    writeSource5075(baseSrcF.path, baseSrc);
    writeSource5075(midSrcF.path, midLeafSrc);
    writeSource5075(appSrcF.path, appSrc);

    auto dep = [](const std::string& obj) {
        bezel::Dependency d;
        d.metadataPath = obj;
        d.objectPath = obj;
        return d;
    };

    {
        bezel::Driver driver;
        if (!driver.compileObject(baseSrcF.path, baseObjF.path).success ||
            !std::ifstream(baseObjF.path).good())
            return -2;
    }
    {
        bezel::Driver driver;
        if (!driver.compileObject(midSrcF.path, midObjF.path,
                                  {dep(baseObjF.path)}).success ||
            !std::ifstream(midObjF.path).good())
            return -2;
    }
    bezel::Driver appDriver;
    if (!bezel::test::compileFileForTest(appDriver, appSrcF.path,
                                         appExeF.path,
                                         {dep(baseObjF.path),
                                          dep(midObjF.path)})
             .success)
        return -1;
    int status = bezel::test::runProgramForTest(appExeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// T1 — ★ THE REPRO. Two consecutive omissions (chain depth 3), cross-module:
// `g.EqV5075(5, 5)` through a `Base5075A`-typed reference into
// `Leaf5075A`'s override -> 91 (pre-#5075-fix: 92).
// // per §14.2 "Constraints on overrides", v0.307
// ===========================================================================
BEZEL_TEST(override_omitted_where_chain_5075,
           two_omission_chain_cross_module_returns_91) {
    int rc = buildImportAndRun5075(dep5075("T1", kChain2Body5075),
                                    app5075("T1", "Base5075A", "Leaf5075A"));
    if (rc == -2) return;  // dependency toolchain unavailable
    BEZEL_EXPECT_EQ(rc, 91);
}

// ===========================================================================
// T2 — same-module twin of T1. Confirms the fix does not depend on the
// cross-module element, and that the climb does not accidentally admit a
// genuinely unconstrained `T`.
// // per §14.2, v0.307
// ===========================================================================
BEZEL_TEST(override_omitted_where_chain_5075,
           two_omission_chain_same_module_returns_91) {
    BEZEL_EXPECT_EQ(
        buildLocalAndRun5075(
            local5075("T2", kChain2Body5075, "Base5075A", "Leaf5075A")),
        91);
}

// ===========================================================================
// T3 — ★ A THREE-OMISSION CHAIN (chain depth 4), cross-module: proves the
// fix climbs an ARBITRARY number of consecutive omissions, not just one
// extra hop past the immediate base.
// // per §14.2, v0.307
// ===========================================================================
BEZEL_TEST(override_omitted_where_chain_5075,
           three_omission_chain_cross_module_returns_91) {
    int rc = buildImportAndRun5075(dep5075("T3", kChain3Body5075),
                                    app5075("T3", "Base5075B", "Grand5075B"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 91);
}

// ===========================================================================
// T4 — CONTROL: the middle override restates its `where` clause explicitly;
// only the leaf omits. Cross-module -> 91 on both sides of the fix (already
// handled by #5039 alone; isolates that the defect is specific to a SECOND
// consecutive omission).
// // per §14.2, v0.307
// ===========================================================================
BEZEL_TEST(override_omitted_where_chain_5075,
           middle_restates_control_cross_module_returns_91) {
    int rc = buildImportAndRun5075(dep5075("T4", kChainRestateBody5075),
                                    app5075("T4", "Base5075C", "Leaf5075C"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 91);
}

// ===========================================================================
// T5 — an OVERLOADED generic method in the chain: `Base5075E` declares both
// the constrained generic `EqV5075E<T>` and a same-arity non-generic private
// `EqV5075E(int, int)` sibling. Pins that `materializeConstraintUpChain`
// climbs to the correct overload (by generic arity, not just name+param-count)
// when it passes through `Mid5075E` to reach `Base5075E`.
// // per §14.2, v0.307
// ===========================================================================
BEZEL_TEST(override_omitted_where_chain_5075,
           overloaded_generic_method_chain_cross_module_returns_91) {
    int rc = buildImportAndRun5075(
        dep5075("T5", kChainOverloadBody5075),
        appOverload5075("T5", "Base5075E", "Leaf5075E"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 91);
}

// ===========================================================================
// T6 — ★ (#5091) DECOY-FIRST overload, the reviewer's exact shape: the
// dependency must compile with zero errors and NO `internal:` diagnostic.
// With the shape-only ancestor match the climb copies the decoy's `new()`
// onto `Leaf5075F.EqV5075F`'s `T`, and codegen reports "internal: codegen
// cannot dispatch member 'Equals' on receiver of type 'T'" on Leaf's
// `a.Equals(b)` (a warning: the slot is degraded, the compile succeeds).
// // per §14.2 "Constraints on overrides", v0.307
// ===========================================================================
BEZEL_TEST(override_omitted_where_chain_5075,
           decoy_first_overload_type_differs_dependency_compiles) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz5075d_");
    TempFile5075 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile5075 libObjF((dir / (id + "_lib.o")).string());
    TempFile5075 errF((dir / (id + ".err")).string());
    writeSource5075(libSrcF.path, kChainDecoyFirstBody5075);
    bezel::Driver driver;
    // Capture the dependency compile's diagnostics (same dup2 pattern as
    // xmod_method_reference_imported_3193_test.cpp): the wrong-constraint
    // symptom is an `internal:` WARNING, so compile success alone would pass
    // on the unfixed code too.
    std::fflush(stderr);
    int saved = ::dup(STDERR_FILENO);
    int sink = ::open(errF.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (sink >= 0) { ::dup2(sink, STDERR_FILENO); ::close(sink); }
    bezel::CompileResult r = driver.compileObject(libSrcF.path, libObjF.path);
    std::fflush(stderr);
    if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
    std::ifstream in(errF.path);
    std::string diags((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    BEZEL_EXPECT_TRUE(r.success);
    BEZEL_EXPECT_EQ(r.errorCount, 0u);
    // The capture itself must have worked, or the next check is vacuous.
    BEZEL_EXPECT_TRUE(sink >= 0 && saved >= 0);
    BEZEL_EXPECT_EQ(diags.find("internal: codegen cannot dispatch member "
                               "'Equals'"),
                    std::string::npos);
    BEZEL_EXPECT_EQ(diags.find("internal:"), std::string::npos);
}

// ===========================================================================
// T7 — ★ (#5091) DECOY-FIRST overload, callable (decoy differs by `ref`):
// cross-module `g.EqV5075G(5, 5)` through a `Base5075G`-typed reference into
// `Leaf5075G`'s override -> 91.
// // per §14.2 "Constraints on overrides", v0.307
// ===========================================================================
BEZEL_TEST(override_omitted_where_chain_5075,
           decoy_first_overload_modifier_differs_cross_module_returns_91) {
    int rc = buildImportAndRun5075(
        dep5075("T7", kChainDecoyRefFirstBody5075),
        "module App5075T7; import Dep5075T7; "
        "class C5075T7 public static method Main(): int "
        "var g: Base5075G := new Leaf5075G(); "
        "return g.EqV5075G(5, 5); "
        "end Main; end C5075T7;");
    // NO `rc == -2` skip here, unlike T1-T5: if the climb picks the decoy,
    // the DEPENDENCY's own compile can fail, which buildImportAndRun5075
    // reports as -2 — a skip would pass this row on the code it rejects.
    BEZEL_EXPECT_EQ(rc, 91);
}



// ===========================================================================
// T8 — ★ 3-MODULE chain that SPANS modules (imported-ancestor branch).
// `Dep3A5075T8` declares `Base5075H` (explicit `where T: IEquatable<T>`);
// `Dep3B5075T8` imports `Dep3A5075T8` and declares `Mid5075H` overriding the
// virtual OMITTING `where`, then `Leaf5075H` overriding `Mid5075H` ALSO
// omitting `where`. App imports both deps and dispatches
// `g.EqV5075H(5, 5)` through a `Base5075H`-typed reference into
// `Leaf5075H`'s override -> 91.
//
// The climb walks Leaf (in Dep3B) -> Mid (in Dep3B, same module as Leaf,
// empty AST `where`) -> Base (in Dep3A, IMPORTED — read from
// `importedGenericConstraints` side table). This is the only row that
// exercises the imported-ancestor branch of
// `materializeConstraintUpChain` at lines 5859-5880 of
// `src/sema/TypeResolution_Checks.cpp`. Without #5091's fix-forward (which
// is the bit that consults the imported side table), the climb stops at
// Mid's empty AST clause, the imported constraint is silently lost, and
// codegen's i32-placeholder fast path takes the wrong branch on
// `a.Equals(b)` -> exit 92, the same wrong-dispatch #5075's repro reports.
//
// // per §14.2 "Constraints on overrides", v0.307
// ===========================================================================
BEZEL_TEST(override_omitted_where_chain_5075,
           three_module_chain_spans_modules_cross_module_returns_91) {
    int rc = buildThreeModuleAndRun5075(
        std::string("module Dep3A5075T8; ") + kChain3ModBaseBody5075,
        std::string("module Dep3B5075T8; import Dep3A5075T8; ") +
            kChain3ModMidLeafBody5075,
        "module App5075T8; import Dep3A5075T8; import Dep3B5075T8; "
        "class C5075T8 public static method Main(): int "
        "var g: Base5075H := new Leaf5075H(); "
        "return g.EqV5075H(5, 5); "
        " end Main; end C5075T8;");
    // NO `rc == -2` skip: if the imported-ancestor branch is broken, the
    // mid-leaf dep's OWN compile can fail when Materialize tries to find
    // a constraint for Leaf's `T` and the climb into the imported Base
    // returns nothing. Reporting -2 would let the row pass on the code it
    // is meant to pin (CLAUDE.md vacuous-oracle trap).
    BEZEL_EXPECT_EQ(rc, 91);
}

}  // namespace bezel::module
