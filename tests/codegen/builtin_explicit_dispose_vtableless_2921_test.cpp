// Spec-derived regression test for issue #2921.
//
// §28.2 (`IDisposable` / disposal exactly once) / §28.9 (Runtime-managed types)
// / §28.3 (auto-drop) / §8.1 (`Object` is the universal root), v0.240.
// stdlib §1, stdlib §3 (`Random`), stdlib §3.1 (`StringBuilder`),
// stdlib §6.5/§24.3 (`EventSource<T>`), stdlib §6.6 (the
// `TaskCompletionSource` family).
//
// THE RULE. stdlib §1, verbatim: "Every class implicitly implements
// `IDisposable`", so `x.Dispose()` is legal on every one of these receivers —
// and stdlib §3 says so of `Random` outright: "`Random` is a class with a no-op
// `Dispose` (its only state is its internal seed; there is nothing to
// release)." Even a no-op `Dispose` leaves an instance to reclaim: §28.9's
// *Trivial-dispose classes* subsection, verbatim, "the compiler frees the
// undischarged value at its owner's release point" — which stdlib §1 restates
// as "A trivial-dispose class's instances are compiler-freed at their owner's
// release point". And §28.2, verbatim: "the first call performs the disposal;
// subsequent calls are no-ops" — so that reclamation must happen exactly once
// across the explicit call and the scope-exit auto-drop.
//
// THE DEFECT. The explicit-`Dispose()` lowering
// (`LlvmBackend_Calls2.cpp`) is a chain of per-built-in arms — `IEnumerator`,
// `Domain`, `Channel`, `ChannelSender`, `Mutex`, `Lock`, `Semaphore`,
// `SemaphoreToken`, `Atomic`, `Buffer`, `Stopwatch`, `FileStream`,
// `CancellationTokenSource`, `Shared`, `Weak`, the collections — ending in the
// generic `emitDisposeAndReclaim`, which loads word 0 as a vtable and calls
// slot 2. A built-in CLASS never carries a Bezel vtable there (word 0 is
// payload), so every built-in the chain did not name fell into a WILD INDIRECT
// JUMP with no diagnostic.
//
// MEASURED on f069645 (the parent commit) — `objdump -dr` of the emitted body
// for `var sb := new StringBuilder(); sb.Dispose();`:
//
//     mov  (%rax),%rcx      <-- the object's first WORD read as a vtable
//     call *0x10(%rcx)      <-- slot 2 through it
//
// which jumped to address 0: `AddressSanitizer: SEGV on unknown address
// 0x000000000000 … pc points to the zero page`, exit 139. Reproduced
// identically for `Random`, `EventSource<int>`, `AsyncEventSource<int>`,
// `TaskCompletionSource<int>` and `OwningTaskCompletionSource<T>`.
//
// AND THE OTHER DIRECTION. `Random` had no resolution on EITHER path:
// `bz_random_free` was declared in the runtime header and referenced by no
// emitter at all, so `using var r := new Random()` emitted allocate-and-return.
// MEASURED under LeakSanitizer, 200-iteration loop: "3200 byte(s) leaked in 200
// allocation(s)", while `Array<int>` and `StringBuilder` in the identical loop
// were clean. So the two spellings were broken in OPPOSITE directions and
// neither could be fixed by copying the other.
//
// THE FIX establishes ONE release per built-in
// (`emitVtablelessBuiltinRelease`) and gives BOTH paths that resolution, gated
// on the single property "does this receiver carry a Bezel vtable?"
// (`common::isVtableLessBuiltinClassName`) rather than on a longer list of
// names. A built-in whose release is not yet known emits NOTHING — a bounded
// leak in place of corrupted control flow (the trade recorded on #2874/#2823).
//
// WHY THESE ORACLES. A zero exit code is not evidence of disposal: #2921's own
// filing initially concluded the `using var` path "gets these right" from
// exit 0, and the emitted instructions showed no release at all. So:
//   * the crash cases assert a DISTINCT non-zero exit value, which a SIGSEGV
//     cannot produce;
//   * every disposal claim is a loop-amplified ASan/LSan run — LSan for the
//     under-release direction, generated-code ASan for the over-release one;
//   * the §28.2 exactly-once case pairs the explicit call WITH the scope-exit
//     auto-drop in one scope, which is precisely where a fix that made the
//     explicit call release without suppressing the auto-drop would turn this
//     SIGSEGV into a double free.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

std::string prog(const std::string& mainBody, const std::string& prelude = "") {
    return "module M; import System.Threading; " + prelude +
           "class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

// A sendable class payload for the owning-generic built-ins.
const char* kPayload =
    "sealed class P "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "end P; ";

}  // namespace

// ---------------------------------------------------------------------------
// stdlib §1: the explicit call is legal on every class. Each of these
// SIGSEGV'd (exit 139) on f069645; the distinct return value cannot be produced
// by a crash.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_explicit_dispose_vtableless_2921, stringbuilder_explicit_dispose_does_not_jump_28_2) {
    BEZEL_EXPECT_EQ(
        run(prog("var sb := new StringBuilder(); sb.Dispose(); return 41;")),
        41);
}

BEZEL_TEST(codegen_explicit_dispose_vtableless_2921, random_explicit_dispose_does_not_jump_28_2) {
    BEZEL_EXPECT_EQ(
        run(prog("var r := new Random(); "
                 "var v := r.Next(1, 3); "          // stdlib §3: Next(min, max)
                 "r.Dispose(); "
                 "if v < 1 then return 9; end; "
                 "return 42;")),
        42);
}

BEZEL_TEST(codegen_explicit_dispose_vtableless_2921, event_source_explicit_dispose_does_not_jump_24_3) {
    BEZEL_EXPECT_EQ(
        run(prog("var e := new EventSource<int>(); e.Dispose(); return 43;")),
        43);
}

BEZEL_TEST(codegen_explicit_dispose_vtableless_2921, async_event_source_explicit_dispose_does_not_jump_24_3) {
    BEZEL_EXPECT_EQ(
        run(prog("var e := new AsyncEventSource<int>(); e.Dispose(); "
                 "return 44;")),
        44);
}

BEZEL_TEST(codegen_explicit_dispose_vtableless_2921, tcs_explicit_dispose_does_not_jump_6_6) {
    BEZEL_EXPECT_EQ(
        run(prog("var t := new TaskCompletionSource<int>(); t.Dispose(); "
                 "return 45;")),
        45);
}

BEZEL_TEST(codegen_explicit_dispose_vtableless_2921, owning_tcs_explicit_dispose_does_not_jump_6_6) {
    BEZEL_EXPECT_EQ(
        run(prog("var t := new OwningTaskCompletionSource<P>(); t.Dispose(); "
                 "return 46;",
                 kPayload)),
        46);
}

// ---------------------------------------------------------------------------
// §28.2 exactly-once. The explicit call AND the `using var` scope-exit auto-drop
// both reach the same receiver in one scope: a fix that made the explicit call
// release without nulling the slot turns #2921's SIGSEGV into a DOUBLE FREE,
// which generated-code ASan reports. 200 iterations so a single-shot fluke
// cannot pass, and LeakSanitizer covers the opposite direction in the same run.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_explicit_dispose_vtableless_2921, stringbuilder_explicit_then_autodrop_exactly_once_28_2) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var i := 0; "
            "while i < 200 do "
            "  using var sb := new StringBuilder(); "
            "  sb.Append(\"z\"); "
            "  sb.Dispose(); "
            "  i := i + 1; "
            "end; "
            "return 0;")),
        0);
}

BEZEL_TEST(codegen_explicit_dispose_vtableless_2921, event_source_explicit_then_autodrop_exactly_once_28_2) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var i := 0; "
            "while i < 200 do "
            "  using var e := new EventSource<int>(); "
            "  e.Dispose(); "
            "  i := i + 1; "
            "end; "
            "return 0;")),
        0);
}

BEZEL_TEST(codegen_explicit_dispose_vtableless_2921, tcs_explicit_then_autodrop_exactly_once_28_2) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var i := 0; "
            "while i < 200 do "
            "  using var t := new TaskCompletionSource<int>(); "
            "  t.Dispose(); "
            "  i := i + 1; "
            "end; "
            "return 0;")),
        0);
}

// ---------------------------------------------------------------------------
// §28.9 "the compiler frees the undischarged value at its owner's release
// point" — the under-release direction, on BOTH spellings.
// On f069645 the `using var` form leaked 3200 bytes in 200 allocations and the
// explicit form crashed before it could leak.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_explicit_dispose_vtableless_2921, random_using_var_autodrop_is_leak_free_28_9) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var acc := 0; var i := 0; "
            "while i < 200 do "
            "  using var r := new Random(); "
            "  acc := acc + r.Next(1, 3); "
            "  i := i + 1; "
            "end; "
            "if acc < 200 then return 5; end; "
            "return 0;")),
        0);
}

BEZEL_TEST(codegen_explicit_dispose_vtableless_2921, random_explicit_dispose_is_leak_free_28_9) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var acc := 0; var i := 0; "
            "while i < 200 do "
            "  var r := new Random(); "
            "  acc := acc + r.Next(1, 3); "
            "  r.Dispose(); "
            "  i := i + 1; "
            "end; "
            "if acc < 200 then return 6; end; "
            "return 0;")),
        0);
}

BEZEL_TEST(codegen_explicit_dispose_vtableless_2921, random_explicit_then_autodrop_exactly_once_28_2) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var i := 0; "
            "while i < 200 do "
            "  using var r := new Random(); "
            "  r.Dispose(); "
            "  i := i + 1; "
            "end; "
            "return 0;")),
        0);
}

// ---------------------------------------------------------------------------
// Guards: the arms the fix routes AROUND must be untouched. `Array<T>` reaches
// the collection arm (bz_array_free) and `Mutex` its own arm
// (bz_mutex_dispose); neither may be captured by the new built-in gate, which
// sits in the terminal `else` after both.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_explicit_dispose_vtableless_2921, collection_explicit_dispose_unchanged_28_2) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var i := 0; "
            "while i < 200 do "
            "  var a := new Array<int>(2); "
            "  a.Add(7); "
            "  a.Dispose(); "
            "  i := i + 1; "
            "end; "
            "return 0;")),
        0);
}

BEZEL_TEST(codegen_explicit_dispose_vtableless_2921, mutex_explicit_then_autodrop_unchanged_28_2) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var i := 0; "
            "while i < 200 do "
            "  using var m := new Mutex(); "
            "  m.Dispose(); "
            "  i := i + 1; "
            "end; "
            "return 0;")),
        0);
}

// A USER class's explicit Dispose still runs its destructor and reclaims the
// block through the vtable — the path whose precondition IS satisfied.
BEZEL_TEST(codegen_explicit_dispose_vtableless_2921, user_class_explicit_dispose_unchanged_28_2) {
    BEZEL_EXPECT_EQ(
        run(prog("var u := new U(); u.Dispose(); return U.Disposed;",
                 "class U "
                 "  public static Disposed: int := 0; "
                 "  public constructor() end constructor; "
                 "  destructor U.Disposed := U.Disposed + 1; end destructor; "
                 "end U; ")),
        1);
}
