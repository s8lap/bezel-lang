// Codegen — issue #3449 (§28.12 asynchronous disposal, §28.9 runtime-managed
// types, §28.3 producers / owned-local discharge, spec v0.283): an `await using
// var` bound at a VTABLE-LESS BUILT-IN head reached the generic `asyncDispose`
// fallback and had the built-in's word 0 read as a class vtable.
//
// THE SHAPE, and why it is structural rather than two broken names.
//
// `registerUsingDisposal` (LlvmBackend_Setup.cpp) is one long chain of arms.
// The `if (ld->isAwaitUsing)` branch sits in the MIDDLE of it and, before this
// fix, ALWAYS RETURNED — through its collection arms, its `IAsyncEnumerator`
// arm, or its generic `asyncDispose` fallback. Everything after that branch was
// therefore unreachable for the `await using` spelling: the ~25 residual
// per-built-in arms (`Subscription`, the serializers, `Stopwatch`, `FileStream`,
// `CancellationTokenSource`, `Mutex`, `Lock`, `Semaphore`, `SemaphoreToken`,
// `Channel`, `ChannelSender`, `Shared`, `Weak`, `Buffer`, the completion
// sources, the event sources, `Task`, `StringBuilder`, the #2921/#2973
// vtable-less arm, …) — AND #2992's END-OF-CHAIN GUARD.
//
// That last part is the point. #2992's guard exists precisely to make a
// built-in that reaches the end of the chain unclaimed LOUD ("the guard makes
// the NEXT omission loud rather than the next leak silent"), and for `await
// using` it could not fire at all. So the whole class was silent, and the next
// forgotten head would have been silent too. Fixing the two heads the issue
// named without restoring the guard's reachability would have left that
// property exactly as broken as it was.
//
// THE FAULT the return produced. With no arm claimed, the decl reached the
// generic fallback with a null `disposeAsyncFn` (`classOfLocal` cannot answer
// for a declaration-less built-in head), which `emitAsyncDisposeAwaitAndFree`
// degrades to `emitDisposeAndReclaim` → `emitDynamicDispose`: *load word 0,
// call slot 2*. A vtable-less built-in is a plain C struct — `bz_stopwatch_t`,
// `bz_cts_t`, `bz_mutex_t`, … — whose word 0 is not a vtable. #2975's
// wild-jump shape; several of the measured faults land on a literal wild `pc`.
//
// MEASURED on 0c5ecd1 (`bezelc --sanitize=address`, LeakSanitizer on), 200
// executions per program, each paired with its plain-`using var` twin — the
// twins differ from the repro BY THE KEYWORD ALONE and every one of them
// returns 247 cleanly on both compilers. TWENTY-FIVE distinct built-in heads
// crashed; the two the issue named were a sample, not the population:
//
//   head                          pre-fix ASan fault                  post-fix
//   Stopwatch                     SEGV @0xd4c8c in bz$async$Run$Resume    247
//   CancellationTokenSource       SEGV @0x0, pc 0x0 (null fn ptr)         247
//   Random                        SEGV                                    247
//   StringBuilder                 SEGV @0x0, pc 0x0                       247
//   Mutex                         SEGV @0x10                              247
//   Lock (from AcquireAsync)      SEGV @0x2, pc 0x2 (WILD JUMP)           247
//   Semaphore                     SEGV @0x11                              247
//   SemaphoreToken                SEGV @0x0, pc 0x0                       247
//   Atomic<int>                   SEGV @0x11                              247
//   AtomicInteger<int>            SEGV @0x11                              247
//   Channel<int>                  SEGV @0x10                              247
//   ChannelSender<int>            SEGV                                    247
//   TaskCompletionSource<int>     SEGV @0x0, pc 0x0                       247
//   OwningTaskCompletionSource    SEGV                                    247
//   EventSource<int>              SEGV @0x10                              247
//   AsyncEventSource<int>         SEGV                                    247
//   SharedEventSource<int>        SEGV                                    247
//   Shared<Box>                   SEGV @0x7b65863e0080, pc = that address 247
//   Weak<Box>                     SEGV                                    247
//   Subscription                  SEGV                                    247
//   Task<int>                     SEGV @0x10                              247
//   OwningTask<Box>               SEGV                                    247
//   FileStream                    SEGV                                    247
//   BinarySerializer              SEGV                                    247
//   JsonSerializer                SEGV                                    247
//
// The `Shared<Box>` row is the one to read twice: `pc` equals a HEAP address,
// i.e. control transferred to attacker-shaped data, not to a null page.
//
// THE FIX is one gate, and it is the same disposition #3437 applied one family
// over: at a `common::isVtableLessBuiltinClassName` head the `isAwaitUsing`
// branch FALLS THROUGH to the representation chain instead of returning into
// the class-shaped fallback. The SPELLING no longer decides the disposal while
// the REPRESENTATION does not. §28.12 (v0.283) states `await using` "is the
// async counterpart of `using var`" and gives it exactly ONE restriction —
// "`await using` is valid only inside an async method or async lambda" — so a
// built-in head is admissible, and having no user `DisposeAsync` to await, the
// `await` is a no-op and the synchronous representation-specific release is the
// whole obligation. That is #2739's recorded ruling, extended to the residual
// built-ins exactly as #3437 extended it to the iterator-format family. No
// sema rejection: one would silently reverse #2739, whose tests pin it.
//
// The predicate is the DERIVED one (`isReservedBuiltinTypeName` minus the
// interface heads), the same #3157 used to split `emitOwnedParamCaptureDispose`
// for the identical reason. It fails SAFE in this direction too: a new built-in
// CLASS joins the set automatically and gets the representation chain, never
// the word-0 read. A null `classOfLocal` is deliberately NOT the discriminator
// — `await using var d: IAsyncDisposable := Make();` has none either and its
// slot really does hold a class object; the built-in INTERFACE heads are
// subtracted from the predicate for exactly that reason and keep the fallback.
//
// THE GUARD IS REACHABLE AGAIN, measured rather than asserted. No head in the
// current population is unclaimed (#2992 measured that, and this lane re-swept
// 25 more heads under `await using` finding no `internal:` note), so no
// shippable program can fire the guard today — that is the guard's whole point.
// Reachability was therefore proven by disabling ONE arm (`if (isSW)` →
// `if (false && isSW)`) in two otherwise-identical compilers and compiling
// `await using var s := new Stopwatch();`:
//
//   compiler                      `await using` note   `using var` note
//   0c5ecd1 + arm disabled              0                   1
//   this fix + arm disabled             1                   1
//
// and the note names the spelling it saw ("internal: `await using var` of
// disposable built-in `Stopwatch` has no disposal arm …"). The head resolution
// the guard uses and the head resolution the new fall-through uses are now ONE
// function, `Emitter::usingVarBuiltinHeadName`, so the set that falls through
// and the set the guard judges cannot drift apart. A second spelling of this
// decision is exactly the duplicated-predicate shape that produced #2921 and
// #2973 in this very chain, and that #2992 was filed about.
//
// ⚠ THE SCOPE IS HEAD-RESOLVABLE DECLARATIONS, not all of them. Sharing one
// resolution is what makes the fall-through set and the guard's set identical;
// it does not make either set complete. A monomorphized GENERIC head has no
// name `usingVarBuiltinHeadName` can produce, so it is invisible to both:
// `await using var s := new T();` at `T = Stopwatch` still takes the word-0
// read. Measured on 0c5ecd1 and on this fix, IDENTICALLY — `await using` SEGV
// @0x10 both sides, plain `using var` 6400 B / 200 allocs leaked both sides
// (the sync chain does not claim it either). Not a regression and not widened
// by this fix; filed as its own shape by the #3449 review. It is the one place
// the twins still disagree, so no row below asserts otherwise.
//
// Every test runs under runAotChecked (generated-code ASan with LSan), so
// `r.ran` is itself the memory-safety assertion, and each program returns a
// distinctive value through `Main` so a silent mis-execution cannot pass.
// per §28.12, §28.9, §28.3 (issue #3449; #3437, #2739, #2992, #3157, #2975)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
// The program's exit code, or a distinctive sentinel when the checked run did
// not complete cleanly (a crash, a leak, a double free) so a failure diff names
// the oracle rather than a bare 0.
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}

// 200 executions of an async body that binds ONE built-in with `kw` and returns
// 5. 200 * 5 = 1000, 1000 mod 251 = 247 — the value every row below expects,
// and the value both spellings produce for every head once the fix is in.
// `extra` carries any module-level declarations the body needs; `pre` any
// statements that must precede the binding (an owner to derive the value from).
std::string builtinHead(const std::string& kw, const std::string& extra,
                        const std::string& pre, const std::string& bind,
                        const std::string& use) {
    return
        "module M; " + extra +
        "class C "
        "  public static async method Inner(): Task<int> "
        "    return 1; "
        "  end Inner; "
        "  public static async method Run(): Task<int> "
        "    " + pre +
        "    " + kw + " " + bind + "; "
        "    " + use +
        "    return 5; "
        "  end Run; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 1..200 do t := t + C.Run().Result; end; "
        "    return t mod 251; "
        "  end Main; "
        "end C;";
}

// A heap-owning class for the `Shared<T>` rows: the handle's release must reach
// the interior, so a mis-routed disposal shows up as an LSan leak as well as a
// fault.
const char* const kBox =
    "class Box "
    "  public owned _v: Array<int>; "
    "  public constructor(n: int) "
    "    _v := new Array<int>(n, 0); "
    "  end constructor; "
    "end Box; ";
}  // namespace

// ── REPRO 1: the issue's first named head. `bz_stopwatch_t` word 0 read as a
// class vtable and slot 2 called: ASan "SEGV on unknown address 0x0000000d4c8c
// ... in bz$async$Run$Resume". // per §28.12, §28.9 (issue #3449)
BEZEL_TEST(await_using_builtin_head_disposal_3449,
           await_using_stopwatch_head_spec_28_12_28_9) {
    BEZEL_EXPECT_EQ(runChecked(builtinHead(
        "await using var", "", "", "s := new Stopwatch()",
        "s.Start(); s.Stop(); ")), 247);
}

// ── PARITY CONTROL: the same program with the plain keyword — clean before and
// after, so the keyword is the only difference. // per §28.10 (issue #3449)
BEZEL_TEST(await_using_builtin_head_disposal_3449,
           using_var_stopwatch_head_control_spec_28_10) {
    BEZEL_EXPECT_EQ(runChecked(builtinHead(
        "using var", "", "", "s := new Stopwatch()",
        "s.Start(); s.Stop(); ")), 247);
}

// ── REPRO 2: the issue's second named head, and the one whose fault was a
// literal null function pointer (`pc 0x0`). // per §28.12, §30.8 (issue #3449)
BEZEL_TEST(await_using_builtin_head_disposal_3449,
           await_using_cancellation_token_source_head_spec_28_12_30_8) {
    BEZEL_EXPECT_EQ(runChecked(builtinHead(
        "await using var", "", "", "cts := new CancellationTokenSource()",
        "cts.Cancel(); ")), 247);
}

// ── PARITY CONTROL for repro 2. // per §28.10 (issue #3449)
BEZEL_TEST(await_using_builtin_head_disposal_3449,
           using_var_cancellation_token_source_head_control_spec_28_10) {
    BEZEL_EXPECT_EQ(runChecked(builtinHead(
        "using var", "", "", "cts := new CancellationTokenSource()",
        "cts.Cancel(); ")), 247);
}

// ── THE #2921 / #2973 ARM, both receivers. These two are the heads whose
// SILENT omission is what #2992 was filed about; they are the reason the
// end-of-chain guard exists at all, so they must have rows here.
// per §28.9, stdlib §2.7 (issue #3449; #2921)
BEZEL_TEST(await_using_builtin_head_disposal_3449,
           await_using_random_head_spec_28_9) {
    BEZEL_EXPECT_EQ(runChecked(builtinHead(
        "await using var", "", "", "r := new Random(42)",
        "_ := r.Next(10); ")), 247);
}

// per §28.9, stdlib §6.4 (issue #3449; #2973)
BEZEL_TEST(await_using_builtin_head_disposal_3449,
           await_using_atomic_head_spec_28_9) {
    BEZEL_EXPECT_EQ(runChecked(builtinHead(
        "await using var", "", "", "a := new Atomic<int>(0)",
        "a.Value := 3; ")), 247);
}

// per §28.9, stdlib §6.4 (issue #3449; #2973)
BEZEL_TEST(await_using_builtin_head_disposal_3449,
           await_using_atomic_integer_head_spec_28_9) {
    BEZEL_EXPECT_EQ(runChecked(builtinHead(
        "await using var", "", "", "a := new AtomicInteger<int>(0)",
        "_ := a.Increment(); ")), 247);
}

// ── stdlib §3.1 StringBuilder — the #1185 arm. // per §28.9 (issue #3449)
BEZEL_TEST(await_using_builtin_head_disposal_3449,
           await_using_string_builder_head_spec_28_9) {
    BEZEL_EXPECT_EQ(runChecked(builtinHead(
        "await using var", "", "", "sb := new StringBuilder()",
        "_ := sb.Append(\"x\"); ")), 247);
}

// ── stdlib §6.2 Mutex, and the `Lock` it hands back. The Lock row matters
// separately: its head is INFERRED from an awaited call, not written at a `new`
// site, so it exercises the initializer-type limb of the head resolution rather
// than the `new`-spelling one — and its pre-fix fault was a wild jump to
// `pc 0x2`. `Acquire()` blocks and is a compile error in an async body
// (stdlib §6.2), so the async form is the only spelling available here.
// per §28.12, stdlib §6.2 (issue #3449)
BEZEL_TEST(await_using_builtin_head_disposal_3449,
           await_using_mutex_head_spec_28_9) {
    BEZEL_EXPECT_EQ(runChecked(builtinHead(
        "await using var", "", "", "m := new Mutex()", "")), 247);
}

// per §28.12, stdlib §6.2 (issue #3449)
BEZEL_TEST(await_using_builtin_head_disposal_3449,
           await_using_lock_head_inferred_spec_28_12) {
    BEZEL_EXPECT_EQ(runChecked(builtinHead(
        "await using var", "", "var m := new Mutex(); ",
        "lk := await m.AcquireAsync()", "m.Dispose(); ")), 247);
}

// ── stdlib §6.3 Semaphore and its permit guard, the second inferred head.
// per §28.12, stdlib §6.3 (issue #3449)
BEZEL_TEST(await_using_builtin_head_disposal_3449,
           await_using_semaphore_token_head_inferred_spec_28_12) {
    BEZEL_EXPECT_EQ(runChecked(builtinHead(
        "await using var", "", "var sem := new Semaphore(1); ",
        "tk := await sem.AcquireAsync()", "sem.Dispose(); ")), 247);
}

// ── stdlib §6.9 Channel<T>. // per §28.9, §30.15 (issue #3449)
BEZEL_TEST(await_using_builtin_head_disposal_3449,
           await_using_channel_head_spec_30_15) {
    BEZEL_EXPECT_EQ(runChecked(builtinHead(
        "await using var", "", "", "ch := new Channel<int>()", "")), 247);
}

// ── stdlib §6.6 TaskCompletionSource<T>. // per §28.9 (issue #3449)
BEZEL_TEST(await_using_builtin_head_disposal_3449,
           await_using_task_completion_source_head_spec_28_9) {
    BEZEL_EXPECT_EQ(runChecked(builtinHead(
        "await using var", "", "", "tcs := new TaskCompletionSource<int>()",
        "tcs.SetResult(1); ")), 247);
}

// ── stdlib §6.5 EventSource<T>, and the `Subscription` it hands back — a third
// inferred head, and the one #2992's chain lists first.
// per §28.9, stdlib §6.5 (issue #3449)
BEZEL_TEST(await_using_builtin_head_disposal_3449,
           await_using_event_source_head_spec_28_9) {
    BEZEL_EXPECT_EQ(runChecked(builtinHead(
        "await using var", "", "", "es := new EventSource<int>()",
        "es.Raise(1); ")), 247);
}

// per §28.9, stdlib §2.11 (issue #3449)
BEZEL_TEST(await_using_builtin_head_disposal_3449,
           await_using_subscription_head_inferred_spec_28_9) {
    BEZEL_EXPECT_EQ(runChecked(builtinHead(
        "await using var", "", "using var es := new EventSource<int>(); ",
        "sub := es.Subscribe(lambda(v: int) => ())", "")), 247);
}

// ── §28.13 Shared<T> over a heap-owning interior. The pre-fix fault here
// transferred control to a HEAP address, and the interior would leak under LSan
// even where it did not; both oracles are live on this row.
// per §28.13, §28.9 (issue #3449)
BEZEL_TEST(await_using_builtin_head_disposal_3449,
           await_using_shared_head_spec_28_13) {
    BEZEL_EXPECT_EQ(runChecked(builtinHead(
        "await using var", kBox, "", "sh := new Shared<Box>(new Box(7))", "")),
        247);
}

// ── PARITY CONTROL for the Shared row. // per §28.13 (issue #3449)
BEZEL_TEST(await_using_builtin_head_disposal_3449,
           using_var_shared_head_control_spec_28_13) {
    BEZEL_EXPECT_EQ(runChecked(builtinHead(
        "using var", kBox, "", "sh := new Shared<Box>(new Box(7))", "")), 247);
}

// ── §28.13 Weak<T>, reached by `Downgrade()` — a fourth inferred head.
// per §28.13 (issue #3449)
BEZEL_TEST(await_using_builtin_head_disposal_3449,
           await_using_weak_head_inferred_spec_28_13) {
    BEZEL_EXPECT_EQ(runChecked(builtinHead(
        "await using var", kBox, "using var sh := new Shared<Box>(new Box(2)); ",
        "w := sh.Downgrade()", "")), 247);
}

// ── §30.5 Task<T>. The task arm lives BELOW the branch and additionally
// consults `taskLocalOwnsFrame`, so this row proves the fall-through reaches an
// arm with its own admission test, not merely the name-matching ones.
// per §30.5, §28.3 (issue #3449)
BEZEL_TEST(await_using_builtin_head_disposal_3449,
           await_using_task_head_spec_30_5) {
    BEZEL_EXPECT_EQ(runChecked(builtinHead(
        "await using var", "", "", "t := C.Inner()", "_ := await t; ")), 247);
}

// ── PARITY CONTROL for the Task row. // per §30.5 (issue #3449)
BEZEL_TEST(await_using_builtin_head_disposal_3449,
           using_var_task_head_control_spec_30_5) {
    BEZEL_EXPECT_EQ(runChecked(builtinHead(
        "using var", "", "", "t := C.Inner()", "_ := await t; ")), 247);
}

// ── #2739 PARITY: a builtin COLLECTION head is claimed by the `isAwaitUsing`
// branch's own collection arm, which returns BEFORE the new fall-through. This
// row pins that the fall-through did not capture that family — it was clean
// before the fix and must stay clean. // per §28.11, stdlib §4.4 (issue #3449; #2739)
BEZEL_TEST(await_using_builtin_head_disposal_3449,
           await_using_array_head_2739_parity_spec_28_11) {
    BEZEL_EXPECT_EQ(runChecked(builtinHead(
        "await using var", "", "", "xs: Array<int> := new Array<int>(3, 1)",
        "_ := xs.Count; ")), 247);
}

// ── THE USER-CLASS CONTROL, and the row that pins what the fix must NOT do.
// A user class is not a built-in head, so it keeps the generic `asyncDispose`
// fallback: §28.12 requires the awaited `DisposeAsync()` to run and the
// destructor body NOT to ("For `await using`, the awaited `DisposeAsync()`
// performs whatever asynchronous cleanup the class defines; the destructor body
// (if any) is **not** also run, because `DisposeAsync` supersedes it" — §28.12
// verbatim, v0.283). The observable is stdout, not an exit code: an exit code
// cannot distinguish "DisposeAsync ran" from "the destructor ran".
// per §28.12 (issue #3449)
BEZEL_TEST(await_using_builtin_head_disposal_3449,
           await_using_user_class_still_awaits_dispose_async_spec_28_12) {
    auto r = runAotChecked(
        "module M; "
        "class Res: IAsyncDisposable "
        "  public async method DisposeAsync(): Task "
        "    Console.WriteLine(\"DA\"); "
        "  end DisposeAsync; "
        "  destructor "
        "    Console.WriteLine(\"DTOR\"); "
        "  end destructor; "
        "end Res; "
        "class C "
        "  public static async method Run(): Task<int> "
        "    await using var u := new Res(); "
        "    return 5; "
        "  end Run; "
        "  public static method Main(): int "
        "    return C.Run().Result; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("DA") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("DTOR") == std::string::npos);
}

// ── The plain-keyword twin of the user-class control: §28.12's other half —
// "For plain `using` (or a synchronous `Dispose()` call) on a class that
// implements both, the destructor body runs synchronously and `DisposeAsync` is
// **not** invoked" (§28.12 verbatim, v0.283). Together the two rows pin that the
// keyword still selects the logical-disposal path for a USER class, which is
// exactly the decision the fix leaves alone. // per §28.12 (issue #3449)
BEZEL_TEST(await_using_builtin_head_disposal_3449,
           using_var_user_class_runs_destructor_control_spec_28_12) {
    auto r = runAotChecked(
        "module M; "
        "class Res: IAsyncDisposable "
        "  public async method DisposeAsync(): Task "
        "    Console.WriteLine(\"DA\"); "
        "  end DisposeAsync; "
        "  destructor "
        "    Console.WriteLine(\"DTOR\"); "
        "  end destructor; "
        "end Res; "
        "class C "
        "  public static async method Run(): Task<int> "
        "    using var u := new Res(); "
        "    return 5; "
        "  end Run; "
        "  public static method Main(): int "
        "    return C.Run().Result; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("DTOR") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("DA") == std::string::npos);
}

// ── THE ONE BUILT-IN WITH A GENUINE ASYNC DISPOSAL, pinned as untouched.
// `TaskScope` is the only reserved built-in CLASS that implements
// `IAsyncDisposable` (stdlib §6.10 — the other three IAsyncDisposable names in
// the library, `IAsyncEnumerator` / `IOwningAsyncEnumerable` /
// `IOwningAsyncEnumerator`, are INTERFACE heads and so are subtracted from
// `isVtableLessBuiltinClassName` by construction). It is therefore the single
// head at which falling through to the synchronous representation chain COULD
// have dropped a real awaited `DisposeAsync` — and it does not, because
// `TaskScope` is claimed by its own dedicated arm which sits ABOVE the
// `isAwaitUsing` branch and returns before the new fall-through is reached
// (#808 put it there for the mirror-image reason: the generic fallback would
// have vtable-disposed a `bz_task_scope_t`). Measured 247 on both compilers.
// per §28.12, stdlib §6.10 (issue #3449; #808)
BEZEL_TEST(await_using_builtin_head_disposal_3449,
           await_using_task_scope_head_unaffected_spec_28_12) {
    BEZEL_EXPECT_EQ(runChecked(builtinHead(
        "await using var", "", "", "sc := new TaskScope()",
        "await sc.Join(); ")), 247);
}

// ── THE `builtinClassSym` SHAPE of the head resolution. `Stopwatch.StartNew()`
// arrives as a resolved TypeRef whose `declaration` is a builtinClassSym with a
// null `decl` — the second of the two shapes `builtinTypeName` accepts, and the
// one `newExprBuiltinHead` deliberately REFUSES (it requires a
// declaration-LESS TypeRef, to keep an imported user class of the same name out
// of the built-in arms, #2991). Without a row here the fall-through would be
// pinned only through the `new`-spelling limb.
// pre-fix: ASan "SEGV on unknown address 0x0000000d5614"; post-fix 247.
// per §28.12, §28.9, stdlib §8.1 (issue #3449; #2991)
BEZEL_TEST(await_using_builtin_head_disposal_3449,
           await_using_stopwatch_from_static_factory_spec_28_9) {
    BEZEL_EXPECT_EQ(runChecked(builtinHead(
        "await using var", "", "", "s := Stopwatch.StartNew()",
        "s.Stop(); ")), 247);
}

// ── THE ANONYMOUS OWNING BINDING. §17.13 / §28.10 make `using var _ := e;`
// (and, §28.12, `await using var _ := e;`) "the one `_` form that owns storage"
// — same scope-end disposal as a named `using var`, no name bound. It is a
// separate AST shape, so it gets its own row: pre-fix ASan "SEGV on unknown
// address 0x0000000000000010", post-fix 247.
// per §17.13, §28.10, §28.12 (issue #3449)
BEZEL_TEST(await_using_builtin_head_disposal_3449,
           await_using_anonymous_binding_builtin_head_spec_17_13_28_10) {
    BEZEL_EXPECT_EQ(runChecked(builtinHead(
        "await using var", "", "", "_ := new Stopwatch()", "")), 247);
}
