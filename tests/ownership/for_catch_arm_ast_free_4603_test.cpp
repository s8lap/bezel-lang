// Ownership — a `try`/`catch` inside a `for` body must not be able to free
// the AST it is walking (issue #4603's §29.12 mutation lint).
//
// THE DEFECT. `MethodOwnership::checkForStmtStructuralMutation` (the §29.12
// mutation-during-iteration lint, Ownership.cpp) walks a `for` body looking
// for a structural call on the iterated binding. Its `BodyWalker` reached a
// `CatchArm` through this shape:
//
//     for (const auto& arm : ts->catches) {
//         if (arm.body) {
//             std::vector<std::unique_ptr<Stmt>> bodyVec;
//             bodyVec.push_back(std::unique_ptr<Stmt>(arm.body.get()));  // <--
//             walk(bodyVec, *s);
//         }
//     }
//
// `CatchArm::body` is a `std::unique_ptr<Stmt>` the AST OWNS. Wrapping
// `arm.body.get()` in a second `unique_ptr` (with no `release()`) makes two
// owning pointers alias one node. The temporary `bodyVec`'s destructor then
// `delete`s a node that is still live in the `SourceFile` — and the CFG,
// built over that same live AST, still holds the freed pointer in
// `block.stmts`. The ownership pass's own `processBlock` walk then reads it.
//
// That read is the crash. Measured on `r23-c28` (v0.334), valgrind:
//
//   Invalid read of size 8
//     at __dynamic_cast (dyncast.cc:53)
//     by MethodOwnership::checkThrowableWhileFieldMovedFrom (Ownership_Checks.cpp:20386)
//     by MethodOwnership::processBlock (Ownership.cpp:2802)
//   Address 0x... is 0 bytes inside a block of size 72 free'd
//     by MethodOwnership::checkForStmtStructuralMutation(...)::BodyWalker::walk
//        (unique_ptr.h:92)
//   Block was alloc'd at
//     by Parser::parseStatementImpl (Parser_Stmts.cpp:755)
//
// i.e. an alloc/free/use chain wholly inside the compiler — a SIGSEGV in
// `bezelc` itself, not a miscompile of the input.
//
// WHAT IS ACTUALLY NECESSARY. The lint only needs a `for` over a collection
// whose BODY contains a `try`/`catch`. Measured isolation matrix on `r23-c28`
// (each cell a real `bezelc` invocation, exit code 139 = SIGSEGV):
//
//   for + try/catch, plain `Array<int>`, no async at all ........ CRASH
//   `await for` + try/catch, no faulting task ................... CRASH
//   try/catch with NO loop ..................................... clean
//   `for` + try/catch, async method ............................. CRASH
//
// So the crash needs no `Task.WhenEach`, no `await for`, no `await` inside the
// `try`, and no faulting task — the `await`-shaped row that first exposed it
// (`task_wheneach_2082.borrow_faulted_task_yielded_caught`) is one instance of
// a much broader defect. The rows below pin the MINIMAL shape, so the next
// reader is not sent looking at async.
//
// THE ORACLE IS THE VALUE, NOT "IT DID NOT CRASH" — the same discipline as
// #4612. Pre-fix the compiler dies, so a bare "did not crash" assertion would
// pass vacuously on one side and prove nothing. Every row below runs the
// program and asserts the exact number it must print; a compiler that SIGSEGVs
// produces no number at all. Fail-without measured per row on `r23-c28`
// (exit 139 on every crashing cell, 100/100 on the async repro).
//
// The lint's own behaviour is unchanged by the fix — the second row re-asserts
// that a structural call inside a CATCH ARM still draws the §29.12 warning, so
// the non-owning walk cannot have silently stopped descending into arms.
//
// per §29.12, v0.334 (the lint this walk implements)

#include <string>

#include "support/aot_support.hpp"         // runAot — the AOT oracle
#include "support/ownership_support.hpp"   // parseAndCheckOwnership
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

// A `for` over an `Array<int>` whose body contains a `try`/`catch`. This is
// the minimal crash: no async, no `await for`, no faulting task, no `await`
// inside the `try`. `xs` is the iterated binding, so the lint's guards
// (collection-typed source, plain identifier root) are all satisfied and the
// BodyWalker actually runs.
const char* kForWithTry =
    "var xs: Array<int> := [1, 2, 3]; "
    "var n := 0; "
    "for var x in xs do "
    "  try n := n + x; "
    "  catch Exception => n := n + 1; "
    "  end; "
    "end; "
    "return n;";

std::string wrap(const std::string& body) {
    return std::string("module M; "
                       "class C "
                       "  public static method Main(): int ") +
           body + "  end Main; end C;";
}

}  // namespace

// THE MINIMAL SHAPE. Pre-fix: `bezelc` SIGSEGVs in
// `checkThrowableWhileFieldMovedFrom` reading the catch arm's freed AST node
// (exit 139), so the program never runs and no exit code is produced.
// Post-fix: 1 + 2 + 3 = 6.
BEZEL_TEST(for_catch_arm_ast_free_4603,
           for_body_with_try_catch_runs_and_sums_the_iteration_values) {
    JitResult r = runAot(wrap(kForWithTry));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);
}

// THE LINT STILL DESCENDS INTO CATCH ARMS. The fix replaces an owning vector
// with a non-owning `const Stmt&`; if that walk had stopped reaching arm
// bodies, a structural call inside an arm would silently stop warning — the
// "silent-acceptance inversion" shape this repo treats as the top defect
// class. Pre- and post-fix this program is ACCEPTED and WARNS at the `Add` in
// the catch arm; the row exists so a future change to the walker cannot drop
// the diagnostic without turning this row red.
//
// The oracle is `parseAndCheckOwnership`, NOT `runAot`: `runAot` fills
// `diagnostics` only on reject, so asserting a WARNING through it on an
// accepted program measures nothing (CLAUDE.md, "vacuous-oracle traps").
// per §29.12, v0.334
BEZEL_TEST(for_catch_arm_ast_free_4603,
           structural_call_in_catch_arm_still_warns_29_12) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    var n := 0; "
        "    for var x in xs do "
        "      try n := n + x; "
        "      catch Exception => xs.Add(9); "
        "      end; "
        "    end; "
        "    return n; "
        "  end Main; "
        "end C;");
    // Accepted (a warning, not an error) — §29.12 keeps the program.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    bool warned = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("modifies the collection this `for` is iterating") !=
            std::string::npos)
            warned = true;
    }
    BEZEL_EXPECT_TRUE(warned);
}