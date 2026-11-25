// Regression test for issue #9156 — a plain, non-virtual indexer SETTER
// (`o[k] := v`) disqualified its RECEIVER from §28.2's checked-mode
// reclamation, so the receiver's block was QUARANTINED (never reclaimed) at
// its scope end: 16 B per receiver — 3200 B in 200 allocations over a
// 200-iteration loop.
//
// THE DEFECT. `Emitter::collectCdFreeEligible`
// (src/codegen/LlvmBackend_Setup.cpp) asks, per local, "may this local's
// block be FREED at its scope end, rather than quarantined?" (§28.2, v0.359:
// checked mode MAY defer a disposed instance's reclamation "so that the flag
// remains readable after disposal" — that is, for a SURVIVING BORROW that may
// still read the disposed flag; #4602 frees the block when no borrow can
// reach it). The walk that answers the question is
// `MoveOnlyArrayTransferWalker`, and its AssignmentStmt arm marked the index
// RECEIVER of every `a[i] := rhs` as "transferred" — the #747 LENS-2 rule,
// which exists for a MOVE-ONLY OWNING INLINE ARRAY's slots (such a store may
// alias an owner into the slot, so the scope-end slot disposal is skipped: a
// leak, never a double-free). The #747 consumer of that set is type-filtered
// (`registerInlineOwnedArrayDisposal` accepts only
// `TypeRef::Kind::InlineArray && arrayOwned`), so the over-marking was INERT
// there. The #4602 consumer is NOT type-filtered: any local in the set loses
// its checked-mode free. So a CLASS receiver of an indexer assignment was read
// as "transferred", and its block was quarantined instead of reclaimed.
//
// §6.6 (v0.359) evaluates the lvalue chain's receiver as a BORROW, and the
// free-eligibility walk's own rule says the same ("receiver of a member
// access, method call or INDEX"). A setter writes THROUGH its receiver: it
// cannot alias an owner into the receiver's own block, and the receiver's own
// disposal cascade (§28.4) still runs. So the index receiver is not a
// transfer.
//
// THE FIX. `MoveOnlyArrayTransferWalker` learns which walk it is running
// (`cdFreeEligibilityWalk`); in the #4602 free-eligibility walk the LENS-2
// index-receiver marking is not applied. The #747 walk is unchanged, so the
// move-only inline array's slot-aliasing protection is untouched.
//
// MEASURED on r30-c1 @ e70e47ff9 (`bezelc --sanitize=address
// --checked-disposal`), the issue's own 200-iteration program:
//
//     Direct leak of 3200 byte(s) in 200 object(s) allocated from:
//         #0 ... in calloc
//         #1 ... in main
//     SUMMARY: AddressSanitizer: 3200 byte(s) leaked in 200 allocation(s).
//
// 16 bytes per receiver — the receiver object's own size (a class with one
// int field is `{ptr vtable, i8 disposed flag, i32 payload}` = 16 bytes; a
// class with five int fields leaks 32 bytes per receiver instead). The leak is
// PER RECEIVER, not per setter call: two setter calls per iteration still leak
// 3200 B / 200 allocations. The GETTER twin of the same program leaks
// nothing — the asymmetry the issue names.
//
// THE ORACLE. Every row runs the program TWICE: once plain (`runAot` /
// `runAotModules`) for the VALUE oracle — the program's own exit code and
// output — and once under generated-code ASan TOGETHER WITH §28.2's
// CHECKED-DISPOSAL MODE (`runAotChecked` / `runAotModulesChecked` with
// `opts.checkedDisposal`) for the MEMORY oracle. Checked-disposal mode is the
// mode the leak lives in: in the unchecked mode the block is reclaimed at the
// scope end either way, so the unchecked oracle cannot see this defect.
//
// ROWS.
//   Row 1 — the issue's own shape: an IMPORTED indexer, a plain setter, a
//           lambda index argument, 200 iterations. RED on the base (3200 B /
//           200 allocations), GREEN with the fix (0 / 0).
//   Row 2 — the SINGLE-MODULE twin (the inline-setter arm). RED on the base,
//           GREEN with the fix.
//   Row 3 — the IMPORTED VIRTUAL setter twin (§23.4). MEASURED: it leaks
//           3200 B / 200 allocations on the base TOO — the defect is the
//           index receiver of an indexer ASSIGNMENT, not the accessor's
//           dispatch kind, so the issue's "non-virtual" wording is narrower
//           than the defect. RED on the base, GREEN with the fix.
//   Row 4 — GREEN CONTROL: the GETTER twin, the path the issue compares
//           against ("the getter path does not leak"). The receiver's block
//           is reclaimed, so the leg is leak-free — on the base and with the
//           fix. It is red under a wrong fix that quarantines the getter path
//           (one that marks every index receiver as transferred in the #4602
//           walk): that turns this row red with 3200 B / 200 allocations.
//
// THE SINGLE-MODULE VIRTUAL twin is NOT a row: its receiver is escape-marked
// by the vtable call (§28.2's "the callee's own stores are marked inside
// it"), so `bz_cd_may_free` keeps its block quarantined on the base AND with
// the fix — the fix changes nothing there, and LeakSanitizer sees the
// quarantined block as still REACHABLE, so a leak assertion on that shape
// would be vacuous.
//
// per §28.2, §28.3, §28.4, §28.9, §23.4, §6.6 (issue #9156)

#include <iostream>
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedImpl;
using bezel::test::runAotModules;
using bezel::test::runAotModulesChecked;

namespace {

// Every diagnostic the harness produced, concatenated — the sanitizer report
// the AOT harness embeds in the diagnostic message when ASan / LSan fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// The VALUE oracle: the program compiled, ran to completion, exited with its
// own exit code and printed exactly this output. The plain (uninstrumented)
// run has no sanitizer, so its exit code is the program's own.
void expectValue(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// The MEMORY oracle: the checked-disposal + ASan leg leaked NOTHING at all,
// and no memory error fired. On the base the same programs leak 3200 bytes in
// 200 allocations (the receiver's block, quarantined instead of reclaimed), so
// rows 1 and 2 are RED there.
void expectNoLeak(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocs
                  << " allocation(s) / " << leakedBytes
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_TRUE(sawSummary == false);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
}

// §28.2's CHECKED-DISPOSAL MODE plus generated-code ASan/LSan — the mode the
// defect lives in, with the leak oracle on.
bezel::codegen::CodegenOptions checkedDisposalAsan() {
    bezel::codegen::CodegenOptions opts;
    opts.checkedDisposal = true;   // §28.2's mode (the ASan/LSan bit is the
                                   // harness's own, `runAotChecked*`).
    return opts;
}

// The imported module's read-write indexer — a getter and a setter over a
// plain function-typed index parameter. `virtual` is the only difference
// between the plain twin (rows 1, 2, 4) and the virtual twin (row 3): §23.4
// makes a virtual indexer's accessor dispatch dynamically.
std::string libModule(const std::string& className, bool isVirtual) {
    return "module " + className + "; "
           "public class FlS" + className + " "
           "  private _last: int := 0; "
           "  public " + (isVirtual ? "virtual " : "") +
           "property [q: () -> int]: int "
           "    get => _last + q(); "
           "    set => do _last := q() + value; end; "
           "  end; "
           "  public constructor() end constructor; "
           "end FlS" + className + ";";
}

// The importing module's program: the statement under test, then a plain
// read of the same receiver (so a fix that reclaimed the receiver too early
// would be caught reading freed memory). 200 iterations, so a per-receiver
// leak is unambiguous.
//
// The arithmetic: the setter stores `q() + value` = 7 + 3 = 10; the getter
// returns `_last + q()` = 10 + 1 = 11. So 200 * 11 = 2200. The getter-only
// twin reads 0 + 1 = 1, so 200 * 1 = 200.
std::string appModule(const std::string& className, const std::string& stmt) {
    return "module App" + className + "; "
           "import " + className + "; "
           "public class Program" + className + " "
           "  public static method Main(): int "
           "    var t := 0; "
           "    for var j in 0..<200 do "
           "      using var fl := new " + className + ".FlS" + className + "(); "
           "      " + stmt + " "
           "      t := t + fl[lambda() => 1]; "
           "    end; "
           "    Console.WriteLine(t); "
           "    return 0; "
           "  end Main; "
           "end Program" + className + ";";
}

// The single-module twin of the imported program: the same read-write
// indexer, declared in the program's own module (the inline-setter arm).
std::string singleModule(const std::string& className, bool isVirtual,
                         const std::string& stmt) {
    return "module " + className + "; "
           "public class FlS" + className + " "
           "  private _last: int := 0; "
           "  public " + (isVirtual ? "virtual " : "") +
           "property [q: () -> int]: int "
           "    get => _last + q(); "
           "    set => do _last := q() + value; end; "
           "  end; "
           "  public constructor() end constructor; "
           "end FlS" + className + "; "
           "public class Program" + className + " "
           "  public static method Main(): int "
           "    var t := 0; "
           "    for var j in 0..<200 do "
           "      using var fl := new FlS" + className + "(); "
           "      " + stmt + " "
           "      t := t + fl[lambda() => 1]; "
           "    end; "
           "    Console.WriteLine(t); "
           "    return 0; "
           "  end Main; "
           "end Program" + className + ";";
}

// ── Row 1 (§28.2 / §28.3 / §6.6): the issue's own shape — an IMPORTED
// indexer, a plain (non-virtual) setter, a lambda index argument, 200
// iterations.
//
// RED on the base (3200 B / 200 allocations: the receiver's block,
// quarantined instead of reclaimed), GREEN with the fix (0 / 0).
BEZEL_TEST(indexer_setter_receiver_block_reclaimed_9156,
           imported_plain_setter_reclaims_the_receiver_block) {
    const std::string cls = "Lib9156I";
    const std::vector<ModuleSource> mods{
        ModuleSource{cls, libModule(cls, /*isVirtual=*/false)},
        ModuleSource{"App9156I",
                     appModule(cls, "fl[lambda() => 7] := 3;")}};
    auto v = runAotModules(mods, "App9156I");
    expectValue(v, 0, "2200\n");
    auto r = runAotModulesChecked(mods, "App9156I", checkedDisposalAsan());
    expectNoLeak(r);
}

// ── Row 2 (§28.2 / §28.3 / §6.6): the SINGLE-MODULE twin — the inline
// setter arm, which the defect hit identically (the issue's program is
// cross-module; this row shows the defect is not an import defect).
//
// RED on the base, GREEN with the fix.
BEZEL_TEST(indexer_setter_receiver_block_reclaimed_9156,
           single_module_plain_setter_reclaims_the_receiver_block) {
    const std::string src =
        singleModule("S9156I", /*isVirtual=*/false, "fl[lambda() => 7] := 3;");
    auto v = runAot(src);
    expectValue(v, 0, "2200\n");
    auto r = runAotCheckedImpl(src, "runAotChecked",
                                /*detectLeaks=*/true, checkedDisposalAsan());
    expectNoLeak(r);
}

// ── Row 3 (§28.2 / §28.3 / §23.4): the IMPORTED VIRTUAL setter twin.
//
// MEASURED on the base: it leaks 3200 B / 200 allocations — the same
// receiver's block, the same defect. The index receiver of an indexer
// ASSIGNMENT is what was over-marked, so the accessor's dispatch kind (plain
// or virtual) does not matter. RED on the base, GREEN with the fix.
//
// It is also the guard that the fix does not reclaim the receiver at the
// SETTER CALL: the program reads the receiver AFTER the setter (the getter
// at the end of the loop body), so a fix that freed the block there would
// turn this row red with a heap-use-after-free under ASan.
BEZEL_TEST(indexer_setter_receiver_block_reclaimed_9156,
           imported_virtual_setter_also_reclaims_the_receiver_block) {
    const std::string cls = "Lib9156V";
    const std::vector<ModuleSource> mods{
        ModuleSource{cls, libModule(cls, /*isVirtual=*/true)},
        ModuleSource{"App9156V",
                     appModule(cls, "fl[lambda() => 7] := 3;")}};
    auto v = runAotModules(mods, "App9156V");
    expectValue(v, 0, "2200\n");
    auto r = runAotModulesChecked(mods, "App9156V", checkedDisposalAsan());
    expectNoLeak(r);
}

// ── Row 4 (GREEN CONTROL — the GETTER twin): the path the issue compares
// against ("the getter path does not leak"). The receiver's block IS
// reclaimed, so the checked-disposal leg is leak-free — on the base and with
// the fix.
//
// It is red under a wrong fix that quarantines the getter path too (one that
// marks EVERY index receiver as transferred in the #4602 walk): that turns
// this row red with 3200 B / 200 allocations.
BEZEL_TEST(indexer_setter_receiver_block_reclaimed_9156,
           getter_twin_still_reclaims_the_receiver_block) {
    const std::string cls = "Lib9156G";
    const std::vector<ModuleSource> mods{
        ModuleSource{cls, libModule(cls, /*isVirtual=*/false)},
        ModuleSource{"App9156G", appModule(cls, "")}};
    auto v = runAotModules(mods, "App9156G");
    expectValue(v, 0, "200\n");
    auto r = runAotModulesChecked(mods, "App9156G", checkedDisposalAsan());
    expectNoLeak(r);
}

}  // namespace
