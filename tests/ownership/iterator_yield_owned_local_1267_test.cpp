// Ownership sema / iterator codegen — #1267 / §29.2 "Iterator yield ownership"
// (v0.226) + §28.10 (axiom A5): `yield return <owned LOCAL>` in an OWNING
// iterator (`IOwningEnumerable<T>` / `IOwningAsyncEnumerable<T>`).
//
// THE SPEC. §29.2's owning bullet: "A **named owned binding** as the yield
// operand is **consumed** here: the operand read is an owned-consuming position
// announced by the declared owning return type — no marker … — and the binding
// is moved-from when the yield resumes." Axiom A5 (§28.10): "`using var` is a
// scope pin: non-reassignable, **never movable out**." The two compose: the
// owning yield of a plain `var` binding is a legal transfer to the consumer; the
// owning yield of a `using var` binding is a move out of a scope pin, which the
// language forbids — exactly as `return b;` under an `owned` return type, an
// `owned` argument, `move b`, `using var y := owned b`, and an arm marker each
// already reject it ("cannot move out of `using var` `x`; `using var` is
// scope-bound", §28.3 diagnostic register).
//
// THE BUG (#1267). The owning-yield consume site (Ownership.cpp, the #1863
// branch) applied `markDisposedByName` to ANY currently-Owned named binding,
// including a `using var`. Marking the pin Disposed silenced sema but changed
// nothing in codegen: the value was still transferred into the enumerator's
// `current_val` and taken by the consumer (`TakeCurrent()` — the consumer's
// `using var u := owned x;` then freed it), while the iterator frame ALSO ran
// the pin's own scope-exit disposal, emitted structurally from the `using var`
// declaration (`Emitter::registerUsingDisposal`, LlvmBackend_Setup.cpp:8008, via
// LlvmBackend_Stmts.cpp:42). Two disposals of one slot:
//
//   site 1 (consumer)  — `using var u := owned x;` scope exit in `Main`
//   site 2 (producer)  — `using var b` scope exit inside `bz$iter$Gen$MoveNext`
//                        (sync) / `bz$asynciter$Gen$Resume` (async)
//
// Observed as an ASan heap-use-after-free: "dispose 1" printed once (site 1),
// then the frame's own disposal read the freed header (site 2) — the SIGSEGV the
// issue reported.
//
// THE FIX. Reject the yield of a `using var` binding from an owning iterator
// (the A5 rejection the other consuming positions already make), naming the
// plain-`var` spelling as the remedy. The frame keeps the pin, so nothing is
// handed out twice and the double-dispose is unreachable rather than merely
// unlikely. The BORROWING iterator is untouched: there the same unmarked read is
// a re-borrow, and §29.2 names `using var l := owned line; yield return l;` as
// the *recommended* warning-free adopt-then-yield spelling — the guard test
// below pins that it still compiles and runs clean.
//
// WHY EACH TEST FAILS WITHOUT THE FIX.
//  - The two reject tests: without the fix the program compiles (no diagnostic
//    at all), so `hasErrors()` is false and the substring match finds nothing.
//    Under `runAotChecked` the same program additionally double-frees.
//  - The plain-`var` accept/behavior tests and the borrowing-iterator guard pass
//    both before and after: they are the over-rejection guard for this change
//    (the fix must narrow to `using var`, not to every named binding) and pin
//    the spelling the new diagnostic recommends.
//
// The issue's first symptom — a BAD_REJECT of the *plain* `var` spelling — was
// already fixed by #1863's owning-yield discharge; `plain_var_*` below are its
// standing regression guards for the LOCAL (as opposed to `owned` parameter)
// case, and they cover all four disposal edges §29.7 names.

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"        // runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/ownership_support.hpp"  // parseAndCheckOwnership — diag pins
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

bool sawDiagnostic(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// Prints one line per disposal so a double-dispose is countable, not merely
// crash-detectable.
const char* kBox =
    "class Box "
    "  public Id: int := 0; "
    "  public constructor(id: int) Id := id; end constructor; "
    "  destructor Console.WriteLine(\"dispose \" + Id.ToString()); "
    "end destructor; "
    "end Box; ";

// Net-zero ctor/dtor counter for the multi-edge disposal trace.
const char* kCounted =
    "class Box "
    "  public static live: int := 0; "
    "  public static dead: int := 0; "
    "  public Id: int := 0; "
    "  public constructor(id: int) Id := id; Box.live := Box.live + 1; "
    "end constructor; "
    "  destructor Box.dead := Box.dead + 1; end destructor; "
    "end Box; ";

// The #1267 repro A2, sync: a `using var` source yielded from an
// IOwningEnumerable<T> iterator. Ill-formed per §29.2 + A5.
const std::string kUsingVarYieldSync =
    std::string("module M; ") + kBox +
    "class C "
    "  public static method Gen(): IOwningEnumerable<Box> "
    "    using var b := new Box(1); "
    "    yield return b; "
    "  end Gen; "
    "  public static method Main(): int "
    "    for var x in Gen() do "
    "      using var u := owned x; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// The #1267 repro A2, async: the same shape through IOwningAsyncEnumerable<T>.
const std::string kUsingVarYieldAsync =
    std::string("module M; ") + kBox +
    "class C "
    "  public static async method Gen(): IOwningAsyncEnumerable<Box> "
    "    using var b := new Box(1); "
    "    yield return b; "
    "  end Gen; "
    "  public static async method Main(): Task<int> "
    "    await for var x in Gen() do "
    "      using var u := owned x; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

const char* kRejectNeedle = "cannot yield `using var`";

}  // namespace

// =====================================================================
// #1267 symptom 2 — the double-dispose. §29.2 + §28.10 axiom A5.
// =====================================================================

// The issue's exact repro A2 (sync). WITHOUT the fix this compiles and the one
// Box is freed twice (consumer `using var u`, then the frame's `using var b`
// scope exit inside MoveNext) — a heap-use-after-free under ASan. WITH the fix
// it is a compile error naming the pin and the plain-`var` remedy.
BEZEL_TEST(iterator_yield_owned_local_1267,
           using_var_yield_owning_iterator_rejected_spec_29_2) {
    JitResult r = runAotChecked(kUsingVarYieldSync);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(sawDiagnostic(r, kRejectNeedle));
    // A rejected program never runs — so the double-dispose is unreachable.
    BEZEL_EXPECT_FALSE(r.ran);
}

// Same, driven through the ownership pass alone so the assertion is on the
// diagnostic and not on the link/run toolchain.
BEZEL_TEST(iterator_yield_owned_local_1267,
           using_var_yield_owning_iterator_diagnostic_spec_28_10_a5) {
    auto r = parseAndCheckOwnership(kUsingVarYieldSync);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(kRejectNeedle) != std::string::npos) {
            found = true;
            break;
        }
    BEZEL_EXPECT_TRUE(found);
}

// The async twin (IOwningAsyncEnumerable<T>): the same A5 rejection. WITHOUT the
// fix the frame's `using var` disposal ran in `bz$asynciter$Gen$Resume` after
// the consumer had already freed the value.
BEZEL_TEST(iterator_yield_owned_local_1267,
           using_var_yield_owning_async_iterator_rejected_spec_29_2) {
    auto r = parseAndCheckOwnership(kUsingVarYieldAsync);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(kRejectNeedle) != std::string::npos) {
            found = true;
            break;
        }
    BEZEL_EXPECT_TRUE(found);
}

// =====================================================================
// #1267 symptom 1 — the plain-`var` spelling (the remedy the diagnostic
// names) is ACCEPTED and disposes exactly once. §29.2 owning bullet.
// =====================================================================

BEZEL_TEST(iterator_yield_owned_local_1267,
           plain_var_yield_disposes_once_spec_29_2) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kBox +
        "class C "
        "  public static method Gen(): IOwningEnumerable<Box> "
        "    var b := new Box(1); "
        "    yield return b; "
        "  end Gen; "
        "  public static method Main(): int "
        "    for var x in Gen() do "
        "      using var u := owned x; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);  // no double-free / leak (ASan+LSan)
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "dispose 1"), 1);
}

BEZEL_TEST(iterator_yield_owned_local_1267,
           plain_var_yield_async_disposes_once_spec_29_2) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kBox +
        "class C "
        "  public static async method Gen(): IOwningAsyncEnumerable<Box> "
        "    var b := new Box(1); "
        "    yield return b; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    await for var x in Gen() do "
        "      using var u := owned x; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "dispose 1"), 1);
}

// §29.7's edges for the accepted spelling: normal completion, early `break`
// (iterator abandoned while suspended), and an exception out of the loop body
// (§28.3 "Exception-path discharge" auto-disposes the iteration variable). The
// same generator drives all three; ctor/dtor counts must net to zero across all
// of them, and LSan proves the abandoned enumerator leaked nothing.
BEZEL_TEST(iterator_yield_owned_local_1267,
           plain_var_yield_every_exit_edge_spec_29_7) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kCounted +
        "class C "
        "  public static method Gen(): IOwningEnumerable<Box> "
        "    var a := new Box(1); "
        "    yield return a; "
        "    var b := new Box(2); "
        "    yield return b; "
        "  end Gen; "
        "  public static method All(): int "
        "    var n := 0; "
        "    for var x in Gen() do using var u := owned x; n := n + u.Id; end; "
        "    return n; "
        "  end All; "
        "  public static method Early(): int "
        "    var n := 0; "
        "    for var x in Gen() do "
        "      using var u := owned x; n := n + u.Id; break; "
        "    end; "
        "    return n; "
        "  end Early; "
        "  public static method Boom(): int "
        "    try "
        "      for var x in Gen() do "
        "        using var u := owned x; "
        "        throw new InvalidOperationException(\"stop\"); "
        "      end; "
        "    catch InvalidOperationException => return 7; "
        "    end; "
        "    return 0; "
        "  end Boom; "
        "  public static method Main(): int "
        "    var a := All(); "        // 1 + 2 = 3, exhaustion
        "    var b := Early(); "      // 1, abandoned at the first suspension
        "    var c := Boom(); "       // 7, unwind out of the loop body
        "    if a <> 3 then return 91; end; "
        "    if b <> 1 then return 92; end; "
        "    if c <> 7 then return 93; end; "
        "    return Box.live - Box.dead; "   // 0 iff every Box disposed once
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// Over-rejection guard: the BORROWING iterator's adopt-then-yield idiom.
// §29.2 borrowed-expression bullet names `using var l := owned line;
// yield return l;` as the recommended warning-free spelling — there the
// unmarked read is a RE-BORROW, not a consume, so the A5 rejection above
// must NOT fire and the frame keeps disposing the pin exactly once.
// =====================================================================

BEZEL_TEST(iterator_yield_owned_local_1267,
           borrowing_iterator_using_var_yield_still_accepted_spec_29_2) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kCounted +
        "class C "
        "  public static method Gen(): IEnumerable<Box> "
        "    using var c1 := new Box(1); "
        "    yield return c1; "
        "    using var c2 := new Box(2); "
        "    yield return c2; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var k := 0; "
        "    for var x in Gen() do k := k + 1; end; "
        "    if k <> 2 then return 90; end; "
        "    return Box.live - Box.dead; "   // 0 iff each disposed exactly once
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
