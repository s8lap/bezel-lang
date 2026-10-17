// #6379 — the first codegen rows whose subject is a MULTI-MODULE program,
// written on `runAotModules` / `runAotModulesChecked`
// (tests/support/aot_support.hpp), the harness #6379 added.
//
// ── WHY THIS FILE EXISTS ────────────────────────────────────────────────────
//
// `runAot` / `runAotChecked` are single-module: `cachedCompile` hands ONE
// source text to `Driver::compileFile` with `deps = {}`. Until #6379 there was
// no sanctioned way to write a codegen row about a program with an `import` in
// it. The two-module machinery was not absent, only duplicated — 376 files
// under tests/ call `Driver::compileObject`, and the ones that build a real
// two-module program each hand-roll the same four steps with its own result
// struct and its own LSan environment string. Two lanes hit that wall
// independently this cycle and fell back on shell probes CI cannot run:
//
//   * #6375 (`8c6410d2`) removed `vtableOf(enumTs)`'s IMPORTED-skip, so it
//     changes behaviour on exactly the imported path, and its only evidence
//     was a hand run of `bezelc -c` + `--import`.
//   * #6373's headline finding is a silent acceptance on `Com.Sub.Box<T>`
//     under `import Com.Sub;` — a DOTTED module name (§2.1), which is a
//     two-module shape by construction.
//
// Both reached the same wall and neither built the harness. This file is the
// harness's first user, and it fails if the harness is wrong.
//
// ── THE SHAPE, AND WHY IT IS PUT WHERE IT IS ───────────────────────────────
//
// #6375's defect lives in `wrapUserClassAsIEnumerable`, whose family
// selection asks whether the fetched enumerator is "not the class the
// structural scan guessed". Two facts about the scan (LlvmBackend_Assign.cpp,
// the `structurallyIsEnumerator` / `scanSymbols` pair) decide whether a
// two-module program can reach the defect at all:
//
//   1. The scan prefers LOCAL symbols and only falls back to
//      `ctx_.importedSymbols()` when no local class exposes MoveNext +
//      Current (fix #3197). So `enumTs` is imported ONLY when the importer
//      itself declares no enumerator-shaped class.
//   2. `enumTs` being imported is precisely the case #6343's deleted skip
//      short-circuited (`vtableOf(enumTs)` is null for an import — an import
//      has no local vtable global), which forced the direct family.
//
// Those two facts are why the rows below are arranged as they are, and why
// row 1 puts `Src` in the DEPENDENCY rather than in the importer: a LOCAL
// `Src` holding an imported `Derived` reaches the discriminator too, but it
// ALSO trips an unrelated cross-module disposal-aliasing defect — the
// wrapper frees the `Derived` at loop end and `Src$Dispose` frees it again,
// a heap-use-after-free in `Src$DisposeCore`, MEASURED and identical at both
// legs of #6375. That is a separate defect from this issue's, so the rows
// avoid the shape rather than inheriting its failure.
//
// ── WHAT EACH ROW PINS, AND AT WHAT VERDICT ────────────────────────────────
//
// Rows are named for what they assert. Row 1 is a regression pin; rows 2-4 are
// coverage, and each says so. A row green at both the base commit and the fix
// is COVERAGE, not a regression pin — saying otherwise would repeat exactly
// the mistake #6375's KNOWN LIMITATIONS block was filed about.
//
//   ROW 1 — imported SUBCLASS enumerator with a local guess class.
//           MEASURED: prints 0 at #6375's base, 6 at the fix, ASan/LSan-clean
//           at both. THE DISCRIMINATING ROW. Base's
//           `enumPtr->vtable != vt$(enumTs)` is true because
//           `vt$Lib1.Derived` and `vt$M1.Local` are different globals, so it
//           takes the `$ad` family and re-dispatches through a CLASS vtable
//           whose slot 2 is `Dispose`. The fix's §29.2 slot-0 self-reference
//           reads false for a class vtable and takes the direct family.
//
//   ROW 2 — the IMPORTED-`enumTs` SKIP path itself (`enumTs` IS the imported
//           `Base`). MEASURED: 6 at both legs, clean at both. Asserted so
//           "removing the imported-skip is not a behaviour flip" stops being
//           a handback sentence and becomes a CI fact. Its comment says why
//           it CANNOT be red — that is a structural property of the deleted
//           skip, not an accident of the spelling chosen.
//
//   ROW 3 — a DOTTED module name (`Com.Sub`): the shape #6373 needs and
//           could not express.
//
//   ROW 4 — TWO-LEVEL delegation across the boundary, the genuine `$ad`
//           family. MEASURED at this parent: 6 at both legs, ASan/LSan-CLEAN
//           at both, under the STRICT harness.
//
// ── A PATH #6375 NAMES THAT IS *NOT* EXPRESSIBLE (measured, not assumed) ───
//
// #6375's KNOWN LIMITATIONS block names two imported-path gaps. The second is
// the null-returning imported `GetEnumerator`: dropping the `vtableOf` skip
// makes the wrapper's slot-0 load reachable for an imported `enumTs`, "so a
// null returned by an imported `GetEnumerator` would fault there". That reads
// as a writable row. It is not, and the reason is worth recording because
// "I could not construct it" is otherwise indistinguishable from "I did not
// try":
//
//   1. `GetEnumerator`'s declared return type is pinned to exactly
//      `IEnumerator<T>` (§11.2 / §12.1 member matching), so it cannot be
//      DECLARED as `IEnumerator<T>?`.
//
//   2. Producing a nullable one is legal — `public static method Nothing():
//      IEnumerator<int>? return null; end Nothing;` compiles and runs
//      (measured: ran=1, exit=0). But RETURNING it from `GetEnumerator`
//      is rejected: "the returned value is nullable but the target type is
//      non-nullable (§7)" — the last diagnostic before the run, at the
//      dependency module's own source location.
//
//   3. The one remaining spelling — a nullable local narrowed by
//      `if maybe <> null then return maybe.GetEnumerator(); end if;` — is
//      compiled but the arm is dead: `maybe` is null, so control reaches the
//      non-null fallback and nothing null ever reaches the wrapper (measured:
//      ran=1, exit=0, stdout 6).
//
// So no Bezel program delivers a null enumerator to this site, imported or
// not. The hazard #6375 names is unreachable in the current language, and the
// right resolution is a NOTE on #6375's KNOWN LIMITATIONS block saying so —
// not a row that cannot fail. Recording it here rather than filing a second
// issue, because the finding IS a spec/harness interaction question rather
// than a defect.
//
// ── ORACLE DISCIPLINE ───────────────────────────────────────────────────────
//
// Every row asserts `r.ran` FIRST and unconditionally. There is NO
// skip-absorbing guard in this file: no `checkedToolchainSkipped(r)` arm, no
// `if (!r.ran) return;`. A two-module build that fails to compile is
// `ran == false` with the driver's own stderr in `diagnostics`, and asserting
// `ran` is what keeps that from reading as a pass.
//
// At THIS commit's parent the base leg of row 1 does not merely print the
// wrong number — it CRASHES, so all three assertions fire and `ran` is
// load-bearing rather than belt-and-braces. (See the row: #6372 landed
// between the tree this file was first measured on and this commit's parent,
// and turned the silent 0 into a SEGV.) `ran` is asserted first precisely
// because it is the one assertion that cannot be absorbed.
//
// SPEC BASIS. §29.1 (`GetEnumerator(): IEnumerator<T>` yields a value in
// `IEnumerator<T>` format), §29.2 (the vtable protocol; every iterator vtable
// SELF-REFERENCES in slot 0, every class vtable carries a type descriptor
// there — #6375's discriminator, and the reason inheritance cannot defeat it),
// §29.11 ("each `GetEnumerator()` call produces an independent enumerator"),
// §2.1 (dotted module names), §2.2 (`import` makes a module's names visible),
// §2.4 / §2.6 (a dependency is an object carrying its §2.4 public-interface
// metadata; the driver links it into the importer's executable), §7
// (nullability — see the NULL-PATH note below), v0.338.
//
// `Base` enumerates 1, 2, 3, so a correct sum is 6.

#include <string>
#include <vector>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;

namespace {

// ── The dependency's enumerator pair, in #6375's own shape ─────────────────
//
// `Derived` adds NO members, so `Base`'s and `Derived`'s vtables have
// identical SHAPES and nothing in the layout says "wrong class" — only the
// identity of the vtable global does. That is what makes the misdispatch a
// soundness bug rather than a typing error, and it is preserved verbatim
// across the module boundary: `Derived` must be a SUBCLASS for row 1 to mean
// anything, and a subclass declared in a dependency is a shape `runAot` could
// not express at all.
std::string libEnumerator(const std::string& module) {
    return std::string("module ") + module + "; "
        "public class Base: IEnumerator<int> "
        "  protected _cur: int := 0; "
        "  public constructor() _cur := 0; end constructor; "
        "  public method GetEnumerator(): IEnumerator<int> return this; "
        "    end GetEnumerator; "
        "  public virtual method MoveNext(): bool "
        "    _cur := _cur + 1; return _cur <= 3; end MoveNext; "
        "  public virtual property Current: int get => _cur; end Current; "
        "end Base; "
        "public class Derived: Base "
        "  public constructor() end constructor; "
        "end Derived; ";
}

// A dependency SOURCE class whose `GetEnumerator` hands back ANOTHER object's
// enumerator, so the returned value is not an instance of the class the
// structural scan guessed. `body` names the field type.
std::string depSrc(const std::string& module, const std::string& fieldType) {
    return libEnumerator(module) +
        "public class Src "
        "  private owned _d: " + fieldType + " := new " + fieldType + "(); "
        "  public method GetEnumerator(): owned IEnumerator<int> "
        "    return _d.GetEnumerator(); "
        "  end GetEnumerator; "
        "end Src; ";
}

// A LOCAL enumerator-shaped class, so the structural scan has a guess at all.
//
// #6375's premise is that `enumTs` is the output of a STRUCTURAL SCAN, not a
// fact about the program; the scan's preference for local symbols is what
// makes the guess here LOCAL while every class whose vtable is consulted at
// run time is IMPORTED. The field layout matches `Base`'s exactly (`_cur: int`
// as the only data), so the DIRECT family — a statically-bound call to
// `Local$MoveNext` on the `Derived` pointer — is correct here. §29.1 makes the
// returned value an `IEnumerator<int>` regardless, and #6375's whole argument
// is that a plain class instance deserves the direct family.
const char* kLocalGuess =
    "public class Local: IEnumerator<int> "
    "  protected _cur: int := 0; "
    "  public constructor() _cur := 0; end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> return this; "
    "    end GetEnumerator; "
    "  public virtual method MoveNext(): bool "
    "    _cur := _cur + 1; return _cur <= 3; end MoveNext; "
    "  public virtual property Current: int get => _cur; end Current; "
    "end Local; ";

// The importer's half: it declares NO enumerator class of its own unless a
// row asks for one, and it for-in's over the dependency's source.
std::string importerMain(const std::string& dep,
                         const std::string& entry, bool withLocalGuess) {
    return std::string("module ") + entry + "; import " + dep + "; " +
           (withLocalGuess ? kLocalGuess : "") +
           "public class M "
           "  public static method Main(): int "
           "    var s := new " + dep + ".Src(); "
           "    var t := 0; "
           "    for var x in s do t := t + x; end; "
           "    Console.WriteLine($\"{t}\"); "
           "    return 0; "
           "  end Main; end M;";
}

// Every row's value assertion. `ran` first, unconditionally; the sum is 6.
void expectSum6(const JitResult& r) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("6") != std::string::npos);
}

}  // namespace

// ══════════════════════════════════════════════════════════════════════════
// ROW 1 — THE DISCRIMINATING ROW. RED at #6375's base (prints 0), green at the
// fix (prints 6). This is the first ASSERTED evidence for a two-module program
// on the path #6375 changed.
//
// The dependency declares `Base` / `Derived` and a source `Src` holding an
// owned `Derived`; the importer declares its OWN enumerator-shaped class
// `Local`, so the scan's guess is `Local` and the runtime enumerator is an
// instance of `Lib1.Derived` — a different vtable global.
//
//   at #6375's BASE — `useAdFamily = (enumPtr->vtable != vt$(enumTs))`, and
//   `vt$M1.Local` is a REAL local vtable global, so the test is NOT skipped
//   (the imported-skip does not apply). `vt$Lib1.Derived != vt$M1.Local`, so
//   the `$ad` family is selected and re-dispatches through a CLASS vtable
//   whose slot 2 is `Dispose` — a void function through an `i1` slot. Sum = 0.
//
//   at the FIX — `useAdFamily = (enumPtr->vtable->[0] == enumPtr->vtable)`,
//   §29.2's own question about the value's format. A class vtable's slot 0 is
//   its type descriptor, so the comparison is false, the DIRECT family runs,
//   and the sum is 6. Subclass-proof by construction: `Derived`'s vtable is a
//   different global with a different slot 0.
//
// MEASURED, both legs, `runAotModulesChecked` (detect_leaks=1):
//
//     base 8c6410d2^ (THIS COMMIT'S PARENT, c8486891)
//         AddressSanitizer: SEGV on unknown address 0x000000248bd8
//         "Hint: PC is at a non-executable region. Maybe a wild jump?"
//         SUMMARY: SEGV bezel in itmap$Lib1.Derived
//         => ran=0, exit=-1, stdout "" — ALL THREE assertions fail.
//
//     fix 8c6410d2 — ran=1, exit=0, stdout "6", no sanitizer report.
//
// ⚠ THE BASE LEG CRASHES HERE, AND THAT IS NOT #6375'S OWN STORY. On the tree
// this file was FIRST measured against (`c7e4581b`, one commit earlier in this
// lane) the same base leg printed a silent "0" with exit 0 — the shape #6375's
// own commit body describes. The difference is **#6372 (`95c1e290`), which
// landed in between**: it made the for-in loop-end disposal go through the
// vtable family the enumerator actually is, so the wrong family is now not
// merely wrong-valued but dispatches a disposal through a bad slot and jumps
// into `itmap$Lib1.Derived`.
//
// So at THIS commit's parent `ran` is a load-bearing part of row 1's evidence,
// not just a hygiene assertion, and the earlier "a silent wrong value, not a
// crash, which is why stdout is the oracle" reading of this row is simply
// false here. Row 1 asserts all three anyway; both legs of the fix make them
// pass, and only the base leg distinguishes which assertion did the work.
//
// Note what makes this row expressible at all: EVERY class whose vtable is
// consulted lives in the dependency. A single-module row can only guess at a
// class it also declares.
BEZEL_TEST(codegen_imported_enumerator_wrapper_family_6379,
           imported_subclass_enumerator_takes_the_direct_wrapper_family_spec_29_2) {
    std::vector<ModuleSource> mods;
    mods.push_back({"Lib1", depSrc("Lib1", "Derived")});
    mods.push_back({"M1", importerMain("Lib1", "M1", /*withLocalGuess=*/true)});
    JitResult r = bezel::test::runAotModulesChecked(std::move(mods), "M1");
    expectSum6(r);
}

// ══════════════════════════════════════════════════════════════════════════
// ROW 2 — THE IMPORTED-`enumTs` SKIP PATH, asserted UNAFFECTED. Coverage, not
// a regression pin; the name says so.
//
// #6375 deleted this skip:
//     if (llvm::GlobalVariable* enumClassVt = vtableOf(enumTs)) { …test… }
// `vtableOf` returns null for an IMPORTED `enumTs` (an import has no local
// vtable global), so at the base the whole discriminator was SKIPPED and a
// delegating imported enumerator was hard-wired to the direct family.
//
// This row puts `enumTs` in the IMPORTED position deliberately: the importer
// declares no local enumerator-shaped class, so the scan finds `Base` through
// `ctx_.importedSymbols()` (#3197) and `enumTs` IS the imported class. The
// runtime enumerator is a `Lib2.Base` instance — `Derived` is present in the
// dependency but never instantiated, and is not a candidate either.
//
// ⚠ THIS ROW IS GREEN AT BOTH LEGS AND IT CANNOT BE MADE RED. That is worth
// stating precisely, because #6375's KNOWN LIMITATIONS block could only report
// it as a hand measurement and had no way to say WHY:
//
//   * At the base an imported `enumTs` skipped the test and took the direct
//     family. `Lib2.Base` IS an ordinary class instance — its vtable's slot 0
//     is `td$Lib2.Base`, not a self-reference — so the fix's discriminator
//     ALSO answers false and ALSO takes the direct family. Same answer.
//
//   * For the skip's removal to change an outcome the value would have to be
//     in §29.11 ITERATOR-VTABLE format, i.e. a delegating `GetEnumerator` whose
//     result has already been wrapped. Then the base's hard-wiring to the
//     direct family is the defect and the fix's `$ad` is the repair — a
//     change in the fix's favour, not a regression this row could demonstrate.
//     Row 4 reaches that family, from the other side.
//
// MEASURED, both legs, `runAotModulesChecked`: ran=1, exit=0, stdout "6", no
// sanitizer report.
//
// So what the row buys: "removing the imported-skip is not a behaviour flip on
// this shape" becomes an asserted CI fact rather than a handback sentence, and
// a future refactor of the imported path has a row that CAN go red.
BEZEL_TEST(codegen_imported_enumerator_wrapper_family_6379,
           imported_guessed_enumerator_class_enumerates_correctly_spec_29_1) {
    std::vector<ModuleSource> mods;
    mods.push_back({"Lib2", depSrc("Lib2", "Base")});
    mods.push_back({"M2", importerMain("Lib2", "M2", /*withLocalGuess=*/false)});
    JitResult r = bezel::test::runAotModulesChecked(std::move(mods), "M2");
    expectSum6(r);
}

// ══════════════════════════════════════════════════════════════════════════
// ROW 3 — A DOTTED MODULE NAME.
//
// #6373's headline finding is a silent acceptance on `Com.Sub.Box<T>` under
// `import Com.Sub;`. §2.1 makes a dotted name the module's own name, so the
// shape is a two-module program by construction — and before #6379 there was
// no way to say so in a row at all. This row is deliberately the SMALLEST
// thing that proves the harness carries a dotted name on disk and through the
// build: it is not a #6373 row (that issue's own file is its author's, in
// another lane), it is the harness proving it can express the shape so the
// next row written for #6373 does not have to.
//
// MEASURED: `2 * 3 = 6` computed by the IMPORTED method. A dotted name that
// silently resolved to nothing would fail to compile or print 0, and either
// way `ran` plus the value catch it. This is also the one shape that would
// have caught a harness that mangled the name on disk (the harness derives
// filenames from module names, so the two spellings are worth pinning).
BEZEL_TEST(codegen_imported_enumerator_wrapper_family_6379,
           dotted_module_name_imports_and_resolves_spec_2_1) {
    std::vector<ModuleSource> mods;
    mods.push_back({"Com.Sub",
                    "module Com.Sub; "
                    "public class Math "
                    "  public static method Twice(n: int): int => n * 2; "
                    "end Math; "});
    mods.push_back({"Main",
                    std::string("module Main; import Com.Sub; "
                                "public class M "
                                "  public static method Main(): int "
                                "    Console.WriteLine("
                                "$\"{Com.Sub.Math.Twice(3)}\"); "
                                "    return 0; "
                                "  end Main; end M;")});
    JitResult r = bezel::test::runAotModulesChecked(std::move(mods), "Main");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("6") != std::string::npos);
}

// ══════════════════════════════════════════════════════════════════════════
// ROW 4 — TWO-LEVEL DELEGATION ACROSS THE BOUNDARY: the genuine `$ad` family.
//
// `Outer.GetEnumerator` delegates to `Inner.GetEnumerator`, which delegates
// to `Base`'s own enumerator. The inner value is a plain class instance while
// the outer one is the result of another delegation, so the wrapper has to
// answer the §29.2 question about a value whose format is not the one a plain
// class instance has. Both classes live in the dependency; the importer keeps
// the local `Local` guess so the discriminator runs at all.
//
// ⚠ THIS ROW USED THE LENIENT HARNESS, AND THAT WAS WRONG AT THIS PARENT.
//
// The first revision ran it under a `runAotModulesCheckedLenient` on the
// reasoning that the 32-byte #6372 leak it measured was pre-existing and would
// make a STRICT row red at both legs. That reasoning was correct against the
// tree this lane first measured on (`c7e4581b`) and wrong against this
// commit's parent: **#6372 (`95c1e290`) landed between them and FIXED that
// leak**. Re-measured here — `runAotModulesChecked` is GREEN, 0 bytes, at this
// parent.
//
// So there is nothing left to suppress, and `Lenient` was disabling leak
// detection on the only row that reaches the genuine `$ad` family. The helper
// was deleted rather than kept for symmetry (a helper with no row behind it is
// surface that reads as coverage and is not), and this row now asserts the
// full dispose-exactly-once contract. The cost of getting this wrong is worth
// stating: the suppressed leak would have been invisible HERE while every
// leak oracle in the suite stayed green — a leak-shaped hole in the one place
// a multi-module row needs one.
//
// What the row pins, at both legs: the enumeration is CORRECT (6) AND the
// program is dispose-exactly-once. A future refactor that made the
// discriminator answer `false` here — sending a genuinely iterator-format
// value down the direct family — would print the wrong sum and go red.
BEZEL_TEST(codegen_imported_enumerator_wrapper_family_6379,
           two_level_delegation_across_modules_enumerates_correctly_spec_29_11) {
    const std::string dep = "Lib4";
    std::vector<ModuleSource> mods;
    mods.push_back({dep,
                    libEnumerator(dep) +
                        "public class Inner "
                        "  private owned _c: Base := new Base(); "
                        "  public method GetEnumerator(): owned IEnumerator<int> "
                        "    return _c.GetEnumerator(); "
                        "  end GetEnumerator; "
                        "end Inner; "
                        "public class Outer "
                        "  private owned _i: Inner := new Inner(); "
                        "  public method GetEnumerator(): owned IEnumerator<int> "
                        "    return _i.GetEnumerator(); "
                        "  end GetEnumerator; "
                        "end Outer; "});
    mods.push_back({"M4", std::string("module M4; import " + dep + "; ") +
                            kLocalGuess +
                            "public class M "
                            "  public static method Main(): int "
                            "    var s := new " + dep + ".Outer(); "
                            "    var t := 0; "
                            "    for var x in s do t := t + x; end; "
                            "    Console.WriteLine($\"{t}\"); "
                            "    return 0; "
                            "  end Main; end M;"});
    JitResult r = bezel::test::runAotModulesChecked(std::move(mods), "M4");
    expectSum6(r);
}