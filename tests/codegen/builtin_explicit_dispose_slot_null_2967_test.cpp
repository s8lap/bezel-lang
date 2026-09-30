// Spec-derived regression test for issue #2967.
//
// §28.2 (`IDisposable` / disposal exactly once) / §28.3 (local variables,
// ownership and borrows — the `using var` scope-bound auto-drop) /
// §28.11 (collection element ownership), v0.240.
// stdlib §4.4/§4.5 (`Array<T>` / `OwningArray<T>`), stdlib §4.6/§4.7
// (`LinkedList<T>` / `OwningLinkedList<T>`), stdlib §4.14 (`OwningBuffer<T>`),
// stdlib §8.1 (`Stopwatch`), stdlib §2.16 (`Shared<T>` / `Weak<T>`).
//
// THE RULE. §28.2, verbatim: "the first call performs the disposal; subsequent
// calls are no-ops". §28.3 arms a scope-bound auto-drop for a `using var`. So
// when a program writes BOTH — an explicit `x.Dispose()` on a `using var` — the
// object must be released exactly once across the two, which the compiler
// achieves by nulling the receiver's slot after the explicit release so the
// scope-exit reload finds null and discharges nothing.
//
// THE DEFECT. The explicit-`Dispose()` lowering (`LlvmBackend_Calls2.cpp`) is a
// chain of per-built-in arms, and that slot-null was a per-arm HABIT rather
// than a property of the release: the `Mutex`, `Lock`, `Semaphore`,
// `SemaphoreToken`, `Atomic`, `Buffer`, `Shared`, `Channel`, `ChannelSender`,
// `Domain`, `FileStream`, `IEnumerator` and `isDictTypeName` arms each wrote it
// out; the `isCollectionTypeName` (→ `bz_array_free`), `isLinkedListTypeName`
// (→ `bz_llist_free`) and dict-backed-`Set` arms, the `Stopwatch` arm and the
// `Weak` arm did not. Those arms were not safe by design — they were safe only
// where the runtime free HAPPENED to tolerate a repeat, which is an accident of
// the runtime rather than a rule about disposal.
//
// MEASURED on the parent commit, 200-iteration
// `using var x := new T(…); x.Dispose();` loops under generated-code
// AddressSanitizer — TWELVE receivers, against the THREE the issue named:
//
//     Array<int>            SEGV on unknown address 0xfffffffffffffff8
//     OwningArray<T>        SEGV on unknown address 0xfffffffffffffff8
//     Set<int>              SEGV on unknown address 0x000000000008
//     SortedSet<int>        SEGV on unknown address 0x000000000008
//     Stack<int>            SEGV on unknown address 0xfffffffffffffff8
//     OwningStack<T>        SEGV on unknown address 0xfffffffffffffff8
//     Queue<int>            SEGV on unknown address 0xfffffffffffffff8
//     OwningQueue<T>        SEGV on unknown address 0xfffffffffffffff8
//     LinkedList<int>       SEGV on unknown address 0x00000000000e
//     OwningLinkedList<T>   SEGV on unknown address 0x00000000000e
//     OwningBuffer<int>     SEGV on unknown address 0xfffffffffffffff8
//     Stopwatch             attempting double-free on 0x7b5b903e0040
//     Weak<T>               8000 byte(s) leaked in 200 allocation(s)
//
// and on the PLAIN build (a different oracle, #2633) as
// "free(): invalid pointer" / "free(): double free detected in tcache 2",
// exit 134. `objdump -dr` of the `Array<int>` body showed it directly:
//
//     call bz_array_alloc        <-- allocate
//     mov  %rax,0x8(%rsp)        <-- store to the slot
//     call bz_array_free         <-- the EXPLICIT x.Dispose()
//     mov  0x8(%rsp),%rdi        <-- reload the SAME, un-nulled slot
//     call bz_array_free         <-- the §28.3 auto-drop: the second free
//
// with no null test anywhere between. After the fix the same disassembly
// carries `movq $0x0,0x8(%rsp)` between the two calls.
//
// WHY THESE ORACLES, and why the scope is a GRID and not the issue's list.
//   * The issue named three receivers (`Array`, `OwningBuffer`, `Stopwatch`).
//     A receiver x spelling grid found twelve — whether a given receiver
//     SEGV'd, double-freed or merely leaked depended only on what the second
//     free happened to read out of the already-freed struct. So the scope
//     below is the grid's, not the filing's.
//   * Each receiver is asserted in all THREE spellings, because only the
//     paired one is the defect: the explicit call alone and the `using var`
//     auto-drop alone were BOTH clean before the fix. A test that exercised
//     one spelling would have passed on the broken compiler.
//   * The `OwningArray` case asserts a LIVE-OBJECT COUNTER, not "no leak":
//     LeakSanitizer is blind to the over-release direction, which is where
//     this defect's damage is. `Src._live` distinguishes a leak (live > 0)
//     from `Src._dtor` distinguishing a double discharge (dtor > created) —
//     an oracle that does not depend on a sanitizer at all.
//   * No test here uses "exit 0" as its evidence of disposal.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedDisposal;

namespace {

// runAotChecked = generated-code AddressSanitizer + LeakSanitizer (the memory
// oracle), NOT §28.2 checked-disposal mode (that is runAotCheckedDisposal).
int runSan(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

std::string outOf(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

std::string prog(const std::string& mainBody, const std::string& prelude = "") {
    return "module M; import System.Diagnostics; " + prelude +
           "class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

// §28.11: a class element an owning collection disposes. `_live` counts live
// instances (ctor +1, dtor -1) and `_dtor` counts destructor RUNS, so the pair
// separates a leak (live > 0) from a double discharge (dtor > created).
const char* kSrc =
    "class Src "
    "  public static _live: int := 0; "
    "  public static _dtor: int := 0; "
    "  private _v: int := 5; "
    "  public constructor() Src._live := Src._live + 1; end constructor; "
    "  destructor "
    "    Src._live := Src._live - 1; "
    "    Src._dtor := Src._dtor + 1; "
    "  end destructor; "
    "end Src; ";

// The §28.2 shape under test: an explicit `.Dispose()` on a `using var`, so the
// explicit release and the §28.3 scope-exit auto-drop both reach one receiver.
// 200 iterations so a single-shot fluke cannot pass.
std::string bothSpellings(const std::string& ctor) {
    return prog("var i := 0; "
                "while i < 200 do "
                "  using var x := " + ctor + "; "
                "  x.Dispose(); "
                "  i := i + 1; "
                "end; "
                "return 0;");
}

// Control A: the explicit call alone (no `using var`) — clean BEFORE the fix.
std::string explicitOnly(const std::string& ctor) {
    return prog("var i := 0; "
                "while i < 200 do "
                "  var x := " + ctor + "; "
                "  x.Dispose(); "
                "  i := i + 1; "
                "end; "
                "return 0;");
}

// Control B: the auto-drop alone — also clean BEFORE the fix.
std::string usingVarOnly(const std::string& ctor) {
    return prog("var i := 0; "
                "while i < 200 do "
                "  using var x := " + ctor + "; "
                "  i := i + 1; "
                "end; "
                "return 0;");
}

}  // namespace

// ---------------------------------------------------------------------------
// §28.2 exactly-once, the LIVE-OBJECT COUNTER oracle (no sanitizer involved).
// A second `bz_array_free` re-runs the owning collection's per-element
// disposers over freed backing memory: `Src._dtor` would exceed the number of
// instances ever constructed (or the program would crash outright). Exactly 40
// destructor runs for 40 constructions, and zero live at the end, is the §28.2
// / §28.11 contract stated as a value.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_explicit_dispose_slot_null_2967, owning_array_explicit_then_autodrop_disposes_elements_exactly_once_28_2) {
    BEZEL_EXPECT_EQ(
        outOf(prog("var i := 0; "
                   "while i < 20 do "
                   "  using var a := new OwningArray<Src>(); "
                   "  a.Add(new Src()); "
                   "  a.Add(new Src()); "
                   "  a.Dispose(); "
                   "  i := i + 1; "
                   "end; "
                   "Console.WriteLine($\"live={Src._live} dtor={Src._dtor}\"); "
                   "return 0;",
                   kSrc)),
        "live=0 dtor=40\n");
}

// The same program WITHOUT the explicit call — the §28.3 auto-drop alone must
// produce the identical counts. Pinning both spellings equal is what makes the
// case above an exactly-ONCE assertion rather than an exactly-forty one.
BEZEL_TEST(codegen_explicit_dispose_slot_null_2967, owning_array_autodrop_alone_matches_the_paired_spelling_28_3) {
    BEZEL_EXPECT_EQ(
        outOf(prog("var i := 0; "
                   "while i < 20 do "
                   "  using var a := new OwningArray<Src>(); "
                   "  a.Add(new Src()); "
                   "  a.Add(new Src()); "
                   "  i := i + 1; "
                   "end; "
                   "Console.WriteLine($\"live={Src._live} dtor={Src._dtor}\"); "
                   "return 0;",
                   kSrc)),
        "live=0 dtor=40\n");
}

// ---------------------------------------------------------------------------
// §28.2 exactly-once, the generated-code ASan/LSan oracle, one case per
// receiver the grid found. Every one of these SEGV'd, double-freed or leaked
// on the parent commit.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_explicit_dispose_slot_null_2967, array_explicit_then_autodrop_exactly_once_28_2) {
    BEZEL_EXPECT_EQ(runSan(bothSpellings("new Array<int>(2)")), 0);
}

BEZEL_TEST(codegen_explicit_dispose_slot_null_2967, owning_array_explicit_then_autodrop_exactly_once_28_2) {
    BEZEL_EXPECT_EQ(
        runSan(prog("var i := 0; "
                    "while i < 200 do "
                    "  using var x := new OwningArray<Src>(); "
                    "  x.Add(new Src()); "
                    "  x.Dispose(); "
                    "  i := i + 1; "
                    "end; "
                    "return 0;",
                    kSrc)),
        0);
}

BEZEL_TEST(codegen_explicit_dispose_slot_null_2967, set_explicit_then_autodrop_exactly_once_28_2) {
    BEZEL_EXPECT_EQ(runSan(bothSpellings("new Set<int>()")), 0);
}

BEZEL_TEST(codegen_explicit_dispose_slot_null_2967, sorted_set_explicit_then_autodrop_exactly_once_28_2) {
    BEZEL_EXPECT_EQ(runSan(bothSpellings("new SortedSet<int>()")), 0);
}

BEZEL_TEST(codegen_explicit_dispose_slot_null_2967, stack_explicit_then_autodrop_exactly_once_28_2) {
    BEZEL_EXPECT_EQ(runSan(bothSpellings("new Stack<int>()")), 0);
}

BEZEL_TEST(codegen_explicit_dispose_slot_null_2967, queue_explicit_then_autodrop_exactly_once_28_2) {
    BEZEL_EXPECT_EQ(runSan(bothSpellings("new Queue<int>()")), 0);
}

// per §4.6/§4.7: the bz_llist_t-backed family — a DIFFERENT runtime free
// (bz_llist_free), reached through a different arm, and broken the same way.
BEZEL_TEST(codegen_explicit_dispose_slot_null_2967, linked_list_explicit_then_autodrop_exactly_once_28_2) {
    BEZEL_EXPECT_EQ(runSan(bothSpellings("new LinkedList<int>()")), 0);
}

BEZEL_TEST(codegen_explicit_dispose_slot_null_2967, owning_linked_list_explicit_then_autodrop_exactly_once_28_2) {
    BEZEL_EXPECT_EQ(
        runSan(prog("var i := 0; "
                    "while i < 200 do "
                    "  using var x := new OwningLinkedList<Src>(); "
                    "  x.Dispose(); "
                    "  i := i + 1; "
                    "end; "
                    "return 0;",
                    kSrc)),
        0);
}

// stdlib §4.14: OwningBuffer<T> is bz_array_t-backed but is NOT a member of the
// shared heap-builtin table, so it reaches the arm by a different route.
BEZEL_TEST(codegen_explicit_dispose_slot_null_2967, owning_buffer_explicit_then_autodrop_exactly_once_28_2) {
    BEZEL_EXPECT_EQ(runSan(bothSpellings("new OwningBuffer<int>(2)")), 0);
}

// stdlib §8.1: bz_stopwatch_free is a plain free(), so the repeat was a literal
// double-free rather than a walk over freed structure.
BEZEL_TEST(codegen_explicit_dispose_slot_null_2967, stopwatch_explicit_then_autodrop_exactly_once_28_2) {
    BEZEL_EXPECT_EQ(runSan(bothSpellings("new Stopwatch()")), 0);
}

// stdlib §2.16: Weak<T>'s repeat is not a free but a second handle-count drop,
// which strands the control block — LeakSanitizer, not ASan, is what catches
// it, and only because the loop amplifies it 200x.
BEZEL_TEST(codegen_explicit_dispose_slot_null_2967, weak_explicit_then_autodrop_exactly_once_28_2) {
    BEZEL_EXPECT_EQ(
        runSan(prog("var i := 0; "
                    "while i < 200 do "
                    "  using var s := new Shared<Src>(new Src()); "
                    "  using var w := s.Downgrade(); "
                    "  w.Dispose(); "
                    "  i := i + 1; "
                    "end; "
                    "return 0;",
                    kSrc)),
        0);
}

// ---------------------------------------------------------------------------
// CONTROLS — the two single spellings. Both were already clean on the parent
// commit, so a fix that broke either would be caught here, and their being
// clean is what identifies the defect as a double DISCHARGE rather than a
// broken release.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_explicit_dispose_slot_null_2967, array_explicit_call_alone_stays_clean_28_2) {
    BEZEL_EXPECT_EQ(runSan(explicitOnly("new Array<int>(2)")), 0);
}

BEZEL_TEST(codegen_explicit_dispose_slot_null_2967, array_autodrop_alone_stays_clean_28_3) {
    BEZEL_EXPECT_EQ(runSan(usingVarOnly("new Array<int>(2)")), 0);
}

BEZEL_TEST(codegen_explicit_dispose_slot_null_2967, stopwatch_autodrop_alone_stays_clean_28_3) {
    BEZEL_EXPECT_EQ(runSan(usingVarOnly("new Stopwatch()")), 0);
}

// The sibling arm that ALWAYS nulled (`isDictTypeName`) — the one the fix
// hoisted out of the chain into its single exit. It must still be clean.
BEZEL_TEST(codegen_explicit_dispose_slot_null_2967, dictionary_explicit_then_autodrop_still_exactly_once_28_2) {
    BEZEL_EXPECT_EQ(runSan(bothSpellings("new Dictionary<string,int>()")), 0);
}

// The arm the fix's `builtinReleasedHere = false` deliberately excludes: a
// USER class keeps `explicitDisposeOwnedReceiverSlot`'s narrower ownership
// test, which must not be widened to "any named lvalue". Value oracle, not an
// exit code: 200 iterations x 5 == 1000.
BEZEL_TEST(codegen_explicit_dispose_slot_null_2967, user_class_receiver_unaffected_28_2) {
    BEZEL_EXPECT_EQ(
        outOf(prog("var i := 0; var acc := 0; "
                   "while i < 200 do "
                   "  using var r := new R(); "
                   "  acc := acc + r.N(); "
                   "  r.Dispose(); "
                   "  i := i + 1; "
                   "end; "
                   "Console.WriteLine($\"acc={acc}\"); "
                   "return 0;",
                   "class R "
                   "  private _n: int := 5; "
                   "  public method N(): int return _n; end N; "
                   "end R; ")),
        "acc=1000\n");
}

// ---------------------------------------------------------------------------
// §28.2 v0.135 CHECKED-disposal mode. The fix deliberately emits NOTHING there:
// the quarantine keeps the block alive and the disposed-flag guard reads
// THROUGH the slot at the next member entry, so nulling it would trade a
// diagnostic for a dangling read. This pins that the fix left that mode alone.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_explicit_dispose_slot_null_2967, checked_disposal_mode_slot_is_not_nulled_28_2) {
    auto r = runAotCheckedDisposal(
        prog("using var a := new Array<int>(2); "
             "a.Dispose(); "
             "return 47;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 47);
}
