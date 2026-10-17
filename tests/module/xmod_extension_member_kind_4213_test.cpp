// Cross-module confirmation for fix #4213 (§26.3, Appendix A.4
// `ExtensionMember` grammar, v0.298) — the default-deny per-member check
// `ExprTC::visitTopDecl`'s `ExtensionDecl` arm now applies is a SEMA-TIME
// rejection, so it fires the same way for a module compiled as a
// standalone LIBRARY (never imported by anything) as it does for one
// compiled directly. This is the stronger guarantee the #4204 review found
// for the sibling `InvariantDecl` check: the library fails at its OWN
// `compileObject` call, before there is ever an object to import — not a
// call-site check that only trips when some importer happens to reference
// the forbidden member.
//
// One representative kind (`ConstDecl`, the issue's own measured repro
// shape) is enough to confirm the check runs identically regardless of
// module role: the fix lives in sema, which runs once per compilation unit
// on its own AST, with no special-casing for "is this module ever
// imported". `tests/sema/extension_member_kind_4213_test.cpp` covers all
// six kinds (plus the bonus `IndexerDecl` case) in-process; this file only
// needs to prove the SAME rejection survives the `compileObject` cross-
// module entry point sema tests never exercise.

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"
#include "test_harness.hpp"

// ── PORT NOTE (#6386, round 2) ───────────────────────────────────────────────
//
// Both rows were hand-rolled: their own `TempFile4213` / `write4213`, their own
// `Dependency` vector, and `std::system` + `WIFEXITED` + `WEXITSTATUS` to decode
// a run. They now call `test::runAotModules`.
//
// ⚠ THIS FILE ALSO LOSES THREE ABSORBING GUARDS, WHICH IS A STRENGTHENING.
//
//   * `if (!std::ifstream(aObj.path).good()) return;`  (the ACCEPT row)
//   * `if (!appResult.success) return;`                  (the ACCEPT row)
//   * `if (status != -1 && WIFEXITED(status))` wrapping the ONE value
//     assertion, so the exit code was checked ONLY on the leg where the run had
//     already succeeded.
//
// Under those guards a build failure produced exactly ONE failure line and a
// signal produced ZERO value assertions. Each row now asserts `r.ran` FIRST and
// UNCONDITIONALLY, then runs every value assertion. No assertion was removed; the
// guards become unreachable by construction because `r.ran` is false in every
// case they caught, and `r.exitCode` carries the value the old wrapper guarded.
//
// ⚠ THE REJECT ROW IS NOW MEASURED FOR ITS REASON, NOT JUST ITS VERDICT. The old
// row asserted only `success == false`, which is the vacuous-oracle shape the
// review contract calls out: ANY error in the library module — including a parse
// error in the extension syntax, an unrelated name collision, a missing
// import — would have turned this row GREEN without the §26.3 per-member check
// ever running. The port adds an assertion on the diagnostic text, so the row
// fails unless the rejection names the forbidden member kind. (The
// `didNotCrash`-equivalent here is `r.ran == false`, asserted via
// `BEZEL_EXPECT_FALSE(r.ran)`.)
//
// The ACCEPT row's control value is unchanged: `w.DoIt()` is `41 + 1`.
//
// TEST NAMES ARE UNCHANGED (#6386 rule 5). No row was added.
//
// Spec: §26.3, A.4 (`ExtensionMember` grammar), §2.5, §2.6, v0.298, v0.338.

namespace bezel::module {
namespace {

// The two library modules, verbatim. `Lib4213Const` declares the forbidden
// `const` member; `Lib4213Ok` is the same class with it removed.
const char* kLibConst4213 =
    "module Lib4213Const; "
    "public class WidgetXC4213 "
    "  public X: int; "
    "end WidgetXC4213; "
    "public extension WidgetXC4213 "
    "  public static const K4213x: int := 42; "
    "  public method DoIt(): int => this.X; "
    "end extension;";

const char* kLibOk4213 =
    "module Lib4213Ok; "
    "public class WidgetXOk4213 "
    "  public X: int; "
    "  public constructor(x: int) "
    "    X := x; "
    "  end constructor; "
    "end WidgetXOk4213; "
    "public extension WidgetXOk4213 "
    "  public method DoIt(): int => this.X + 1; "
    "end extension;";

// The importer for the ACCEPT row, verbatim.
const char* kAppOk4213 =
    "module App4213; "
    "import Lib4213Ok; "
    "class C4213xok public static method Main(): int "
    "  var w := new WidgetXOk4213(41); "
    "  return w.DoIt(); "
    "end Main; end C4213xok;";

// REJECT — a LIBRARY module (never imported here) whose `public extension`
// declares a `const` member must fail at its own compile, exactly
// as `bezelc -c` alone does for the same source (the issue's own repro).
// Before the fix this call returned `success == true` with zero
// diagnostics and a real `.o` on disk.
//
// `kLibConst4213` is supplied as the ENTRY module (nothing imports it), which is
// the point of the row: the rejection must fire from the library's own
// compilation unit, with no importer in the program at all.
BEZEL_TEST(xmod_extension_member_kind_4213,
           library_module_const_in_extension_rejected_at_own_compile_26_3) {
    auto r = test::runAotModules({{"Lib4213Const", kLibConst4213}},
                                 "Lib4213Const");

    // The verdict: the program must NOT have run. `ran == false` also covers the
    // old row's "no `.o` on disk" assertion — a rejected entry module never
    // reaches the link, so no object exists to import.
    BEZEL_EXPECT_FALSE(r.ran);

    // ⚠ THE REASON, NOT JUST THE VERDICT. The old row asserted only
    // `success == false`, which is the vacuous-oracle shape: ANY error in this
    // library — including an unrelated one, or a parse failure in the extension
    // syntax — would have turned the row GREEN without the §26.3 per-member
    // check ever running. Both needles below are asserted.
    //
    // The `const` needle is the one that pins the KIND: src/sema/ExprTypecheck.cpp
    // builds the message as "an extension block may not declare " + article +
    // " `" + kind + "` member", and the `ConstDecl` arm sets kind = "const".
    //
    // The §26.3 needle is what rules out the PRE-EMPTING CHECK. The extension
    // arm has an EARLIER rule — "extension blocks may not declare `static`
    // members" — which reads `MethodDecl` and `PropertyDecl` modifiers only.
    // The offending member here is `public static const K4213x`, so if that
    // earlier rule had been widened to `ConstDecl` it would fire first and this
    // row would be green for the wrong reason. Asserting the `const` needle is
    // what makes the row fail loudly in that case.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "an extension block may not declare a `const` member"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§26.3"));
}

// ACCEPT (control) — the SAME library module with the forbidden `const`
// member removed compiles and links cleanly as a dependency, and an
// importer can call its legitimate extension method — confirming this
// fix's rejection is specific to the forbidden member, not to library-role
// compilation or `public extension` in general.
BEZEL_TEST(xmod_extension_member_kind_4213,
           library_module_without_const_accepted_and_importable_26_3) {
    auto r = test::runAotModules({
        {"Lib4213Ok", kLibOk4213},
        {"App4213", kAppOk4213},
    }, "App4213");

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

}  // namespace
}  // namespace bezel::module
