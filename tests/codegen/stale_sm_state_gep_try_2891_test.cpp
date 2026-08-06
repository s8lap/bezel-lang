// Codegen — issue #2891 (§31.2 "`try`/`catch`/`finally`", §30.1 "`async`
// methods", §29.3 "Iterator methods", §29.7 "Disposal of partially-consumed
// iterators", v0.240): an ORDINARY program failed to compile —
//
//   Referring to an instruction in another function!
//     store i32 -1, ptr %3, align 4
//   bezel codegen: IR verification failed (malformed IR emitted)
//
// — whenever a plain method (or a constructor, or a destructor) whose body
// contains a `try` with a `finally` was emitted after any state machine.
//
// ROOT CAUSE. `smStateGEP_` is an `llvm::Instruction` — the `CreateStructGEP`
// on the resume function's frame argument, published by `emitResumeDispatch`
// (LlvmBackend_Iterators.cpp ~387). None of the three state-machine emitters
// clears it when it finishes: `emitIteratorMethod` (~1541),
// `emitAsyncMethod` (LlvmBackend_Async.cpp ~1340) and
// `emitAsyncIteratorMethod` (~2575) each clear `smStateStruct_`/`smSelf_` and
// leave the GEP (and `smKind_`) behind. `lowerTry` then read the member
// directly at two sites — the §29.7 "mark the iterator done" store on the
// unwind edge, and the #721 mode-4 `yield break` dispatch — so an ordinary
// method's `finally` emitted a `store i32 -1` through a pointer defined in a
// DIFFERENT function. Where the previous emitter was an ITERATOR
// (`smKind_ == Yield`/`AsyncYield`, likewise never reset) the mode-4 dispatch
// opened too and added a second verifier complaint, "Function return type does
// not match operand type of return inst!", from the `ret i1 false` it emits.
//
// The fix routes both `lowerTry` reads through the new `smStateGEPHere()`
// accessor (LlvmBackend_impl.hpp), which returns the GEP only when its DEFINING
// function is the one being emitted — the same category-blind shape as #2872's
// `disposerEntryExcHere()`, gating on the LLVM property actually required
// rather than on an enumeration of the emitters that fail to clear the member.
//
// WHY MORE THAN ONE ORDERING IS TESTED. The body loop iterates
// `std::unordered_map<sema::MethodSymbol*, llvm::Function*> methodFns_`
// (LlvmBackend_impl.hpp) — POINTER-HASH order, not declaration order — so
// which of two sibling METHODS is emitted first is not fixed by the source.
// Three of the cases below do not depend on that at all and pin the defect
// deterministically: a CONSTRUCTOR body and a DESTRUCTOR body are emitted from
// loops that run after the whole method loop, and an iterator's own Dispose
// body (which re-lowers user `finally` bodies, LlvmBackend_Iterators.cpp ~2007)
// is emitted after its MoveNext inside a single `emitIteratorMethod` call.
//
// Measured on the unpatched tree: every case below except the two marked
// "compiled before the fix too" failed to compile with the verifier error.

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ---------------------------------------------------------------------------
// §31.2 / §30.1 — the issue's own repro. A class with a plain method whose body
// is §31.2's "only a `finally`" form, and an `async` method declared after it.
// `finally` runs on the normal edge, so 0 +1 +10 == 11.
BEZEL_TEST(stale_sm_state_gep_try_2891, plain_try_finally_then_async_method) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class C\n"
        "  public static method Plain(): int\n"
        "    var n := 0;\n"
        "    try\n"
        "      n := n + 1;\n"
        "    finally\n"
        "      n := n + 10;\n"
        "    end;\n"
        "    return n;\n"
        "  end Plain;\n"
        "  public static async method Tick(): Task<int>\n"
        "    return 1;\n"
        "  end Tick;\n"
        "  public static method Main(): int\n"
        "    return C.Plain();\n"
        "  end Main;\n"
        "end C;\n"), 11);
}

// §31.2 / §22.10 — ORDER-INDEPENDENT pin #1: a CONSTRUCTOR body containing the
// same `try`/`finally`. Constructor bodies are emitted from the `ctorFns_` loop,
// which runs after the whole `methodFns_` loop, so the async method's frame is
// always still published when this body is lowered.
BEZEL_TEST(stale_sm_state_gep_try_2891, constructor_try_finally_with_async_method) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class Box\n"
        "  public N: int;\n"
        "  public constructor()\n"
        "    var n := 0;\n"
        "    try\n"
        "      n := n + 1;\n"
        "    finally\n"
        "      n := n + 10;\n"
        "    end;\n"
        "    N := n;\n"
        "  end constructor;\n"
        "end Box;\n"
        "class C\n"
        "  public static async method Tick(): Task<int>\n"
        "    return 1;\n"
        "  end Tick;\n"
        "  public static method Main(): int\n"
        "    using var b := new Box();\n"
        "    return b.N;\n"
        "  end Main;\n"
        "end C;\n"), 11);
}

// §31.2 / §28.4 — ORDER-INDEPENDENT pin #2: a DESTRUCTOR body containing the
// `try`/`finally`. Disposer bodies are emitted from their own loop after the
// method and constructor loops. This is the neighbour shape of #2872
// (`disposerEntryExc_`), and confirms the two fixes are independent: the
// resource is still disposed exactly once (Live returns to 0), so
// 11 (Plain) + 1 (Live inside the scope) + 0 (Live after it) == 12.
BEZEL_TEST(stale_sm_state_gep_try_2891, destructor_try_finally_with_async_method) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class Res\n"
        "  public static Live: int := 0;\n"
        "  public constructor()\n"
        "    Res.Live := Res.Live + 1;\n"
        "  end constructor;\n"
        "  destructor\n"
        "    try\n"
        "      Res.Live := Res.Live - 1;\n"
        "    finally\n"
        "      Res.Live := Res.Live + 0;\n"
        "    end;\n"
        "  end destructor;\n"
        "end Res;\n"
        "class C\n"
        "  public static method Plain(): int\n"
        "    var n := 0;\n"
        "    try\n"
        "      n := n + 1;\n"
        "    finally\n"
        "      n := n + 10;\n"
        "    end;\n"
        "    return n;\n"
        "  end Plain;\n"
        "  public static async method Tick(): Task<int>\n"
        "    return 1;\n"
        "  end Tick;\n"
        "  public static method Main(): int\n"
        "    var acc := 0;\n"
        "    do\n"
        "      using var r := new Res();\n"
        "      acc := acc + Res.Live;\n"
        "    end;\n"
        "    return C.Plain() + acc + Res.Live;\n"
        "  end Main;\n"
        "end C;\n"), 12);
}

// §29.7 — ORDER-INDEPENDENT pin #3, and the one that needs no `async` method at
// all: an iterator abandoned by `break` is disposed, and §29.7 requires its
// pending `finally` to run. That replay lowers the user `finally` body into the
// DISPOSE function while `smStateGEP_` still points into MoveNext — so a `try`
// nested inside that `finally` hit the same cross-function store. The finally
// runs exactly once (+1 then +10), so 1 (the single yielded value) + 11 == 12.
BEZEL_TEST(stale_sm_state_gep_try_2891, iterator_dispose_replays_nested_finally) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class C\n"
        "  public static Trace: int := 0;\n"
        "  public static method Nums(): IEnumerable<int>\n"
        "    try\n"
        "      yield return 1;\n"
        "      yield return 2;\n"
        "    finally\n"
        "      try\n"
        "        C.Trace := C.Trace + 1;\n"
        "      finally\n"
        "        C.Trace := C.Trace + 10;\n"
        "      end;\n"
        "    end;\n"
        "  end Nums;\n"
        "  public static method Main(): int\n"
        "    var s := 0;\n"
        "    for var x in C.Nums() do\n"
        "      s := s + x;\n"
        "      if x = 1 then\n"
        "        break;\n"
        "      end;\n"
        "    end;\n"
        "    return s + C.Trace;\n"
        "  end Main;\n"
        "end C;\n"), 12);
}

// §29.3 — the ITERATOR neighbour of the primary repro. Here the stale frame also
// carried `smKind_ == Yield`, which opened `lowerTry`'s mode-4 (`yield break`)
// dispatch inside an `int`-returning method and emitted `ret i1 false` — the
// second verifier complaint the issue reports. 11 (Plain) + 3 (1+2) == 14.
BEZEL_TEST(stale_sm_state_gep_try_2891, sync_iterator_and_plain_try_finally) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class C\n"
        "  public static method Plain(): int\n"
        "    var n := 0;\n"
        "    try\n"
        "      n := n + 1;\n"
        "    finally\n"
        "      n := n + 10;\n"
        "    end;\n"
        "    return n;\n"
        "  end Plain;\n"
        "  public static method Nums(): IEnumerable<int>\n"
        "    yield return 1;\n"
        "    yield return 2;\n"
        "  end Nums;\n"
        "  public static method Main(): int\n"
        "    var s := 0;\n"
        "    for var x in C.Nums() do\n"
        "      s := s + x;\n"
        "    end;\n"
        "    return C.Plain() + s;\n"
        "  end Main;\n"
        "end C;\n"), 14);
}

// §30.7 — the ASYNC-ITERATOR arm of the same axis (`smKind_ == AsyncYield`).
BEZEL_TEST(stale_sm_state_gep_try_2891, async_iterator_and_plain_try_finally) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class C\n"
        "  public static method Plain(): int\n"
        "    var n := 0;\n"
        "    try\n"
        "      n := n + 1;\n"
        "    finally\n"
        "      n := n + 10;\n"
        "    end;\n"
        "    return n;\n"
        "  end Plain;\n"
        "  public static async method Nums(): IAsyncEnumerable<int>\n"
        "    yield return 1;\n"
        "    yield return 2;\n"
        "  end Nums;\n"
        "  public static async method Main(): Task<int>\n"
        "    var s := 0;\n"
        "    await for var x in C.Nums() do\n"
        "      s := s + x;\n"
        "    end;\n"
        "    return C.Plain() + s;\n"
        "  end Main;\n"
        "end C;\n"), 14);
}

// §29.6 — ACCEPT-CONTROL for the narrowing the fix performs. `smStateGEPHere()`
// closes `lowerTry`'s mode-4 dispatch outside the state machine; this pins that
// it still OPENS inside one. §29.6 admits `yield break` in a `try` whose
// statement has only a `finally`, and §31.2 requires that `finally` to run on
// the way out: 1 (the yielded value) + 7 (the finally) + 11 (Plain) == 19.
BEZEL_TEST(stale_sm_state_gep_try_2891, yield_break_through_finally_still_runs_it) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class C\n"
        "  public static Trace: int := 0;\n"
        "  public static method Nums(): IEnumerable<int>\n"
        "    try\n"
        "      yield return 1;\n"
        "      yield break;\n"
        "    finally\n"
        "      C.Trace := C.Trace + 7;\n"
        "    end;\n"
        "  end Nums;\n"
        "  public static method Plain(): int\n"
        "    var n := 0;\n"
        "    try\n"
        "      n := n + 1;\n"
        "    finally\n"
        "      n := n + 10;\n"
        "    end;\n"
        "    return n;\n"
        "  end Plain;\n"
        "  public static async method Tick(): Task<int>\n"
        "    return 1;\n"
        "  end Tick;\n"
        "  public static method Main(): int\n"
        "    var s := 0;\n"
        "    for var x in C.Nums() do\n"
        "      s := s + x;\n"
        "    end;\n"
        "    return s + C.Trace + C.Plain();\n"
        "  end Main;\n"
        "end C;\n"), 19);
}

// §31.2 / §30.1 — ACCEPT-CONTROL: `return` out of a `try`/`finally` INSIDE the
// async method still routes through the finally and completes the task with the
// value computed before it (mode 1 / mode 5, both untouched by the fix).
// Tick returns 2+30 == 32; 11 (Plain) + 32 == 43.
BEZEL_TEST(stale_sm_state_gep_try_2891, async_return_through_its_own_finally) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class C\n"
        "  public static method Plain(): int\n"
        "    var n := 0;\n"
        "    try\n"
        "      n := n + 1;\n"
        "    finally\n"
        "      n := n + 10;\n"
        "    end;\n"
        "    return n;\n"
        "  end Plain;\n"
        "  public static async method Tick(): Task<int>\n"
        "    var m := 0;\n"
        "    try\n"
        "      m := m + 2;\n"
        "      return m + 30;\n"
        "    finally\n"
        "      m := m + 20;\n"
        "    end;\n"
        "  end Tick;\n"
        "  public static async method Main(): Task<int>\n"
        "    using var t := C.Tick();\n"
        "    var r := await t;\n"
        "    return C.Plain() + r;\n"
        "  end Main;\n"
        "end C;\n"), 43);
}

// §28.3 / §28.10 / §31.2 — the DISPOSAL dimension, under ASan/LSan: `lowerTry`
// is what the stale pointer reached, so pin that both the normal edge and the
// EXCEPTION edge of a `try`/`finally` holding a `using var` still dispose it
// exactly once. Two objects are made; Live must be 0 at the end, so any missing
// or duplicated disposal moves the result by a multiple of 1000.
// Plain == 1 + 10 == 11; Throwing == 1 + 10 + 100 == 111; total 122.
BEZEL_TEST(stale_sm_state_gep_try_2891, using_var_in_try_finally_disposed_once) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M;\n"
        "class Res\n"
        "  public static Live: int := 0;\n"
        "  public static Made: int := 0;\n"
        "  public constructor()\n"
        "    Res.Live := Res.Live + 1;\n"
        "    Res.Made := Res.Made + 1;\n"
        "  end constructor;\n"
        "  destructor\n"
        "    Res.Live := Res.Live - 1;\n"
        "  end destructor;\n"
        "end Res;\n"
        "class C\n"
        "  public static method Plain(): int\n"
        "    var n := 0;\n"
        "    try\n"
        "      using var r := new Res();\n"
        "      n := n + Res.Live;\n"
        "    finally\n"
        "      n := n + 10;\n"
        "    end;\n"
        "    return n;\n"
        "  end Plain;\n"
        "  public static method Throwing(): int\n"
        "    var n := 0;\n"
        "    try\n"
        "      try\n"
        "        using var r := new Res();\n"
        "        n := n + Res.Live;\n"
        "        throw new InvalidOperationException(\"boom\");\n"
        "      finally\n"
        "        n := n + 10;\n"
        "      end;\n"
        "    catch\n"
        "      InvalidOperationException => n := n + 100;\n"
        "    end;\n"
        "    return n;\n"
        "  end Throwing;\n"
        "  public static async method Tick(): Task<int>\n"
        "    return 1;\n"
        "  end Tick;\n"
        "  public static method Main(): int\n"
        "    var a := C.Plain();\n"
        "    var b := C.Throwing();\n"
        "    return a + b + Res.Live * 1000 + (Res.Made - 2) * 1000;\n"
        "  end Main;\n"
        "end C;\n"), 122);
}

// §27.3 — a block-bodied LAMBDA carrying the `try`/`finally`, alongside an
// `async` method. Lambda bodies are emitted from expression lowering, so this
// covers the nested-emitter arm of the axis. 11 (Plain) + 105 (5 + 100) == 116.
BEZEL_TEST(stale_sm_state_gep_try_2891, lambda_try_finally_with_async_method) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class C\n"
        "  public static method Plain(): int\n"
        "    var n := 0;\n"
        "    try\n"
        "      n := n + 1;\n"
        "    finally\n"
        "      n := n + 10;\n"
        "    end;\n"
        "    return n;\n"
        "  end Plain;\n"
        "  public static method Lam(): int\n"
        "    var f := lambda(a: int): int => do\n"
        "      var m := 0;\n"
        "      try\n"
        "        m := m + a;\n"
        "      finally\n"
        "        m := m + 100;\n"
        "      end;\n"
        "      return m;\n"
        "    end;\n"
        "    return f(5);\n"
        "  end Lam;\n"
        "  public static async method Tick(): Task<int>\n"
        "    return 1;\n"
        "  end Tick;\n"
        "  public static method Main(): int\n"
        "    return C.Plain() + C.Lam();\n"
        "  end Main;\n"
        "end C;\n"), 116);
}

// §30.1 — accept-control that compiled before the fix too (the async method
// happened to be emitted after the plain one): the fix must not change it.
BEZEL_TEST(stale_sm_state_gep_try_2891, async_method_declared_before_still_works) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class C\n"
        "  public static async method Tick(): Task<int>\n"
        "    return 1;\n"
        "  end Tick;\n"
        "  public static method Plain(): int\n"
        "    var n := 0;\n"
        "    try\n"
        "      n := n + 1;\n"
        "    finally\n"
        "      n := n + 10;\n"
        "    end;\n"
        "    return n;\n"
        "  end Plain;\n"
        "  public static method Main(): int\n"
        "    return C.Plain();\n"
        "  end Main;\n"
        "end C;\n"), 11);
}

// §31.2 — accept-control that compiled before the fix too: a `try` with only a
// `catch` reaches neither consumer (no `finally` block is emitted), so the arm
// still selects and runs. 1 + 4 == 5.
BEZEL_TEST(stale_sm_state_gep_try_2891, try_catch_without_finally_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class C\n"
        "  public static method Plain(): int\n"
        "    var n := 0;\n"
        "    try\n"
        "      n := n + 1;\n"
        "      throw new InvalidOperationException(\"x\");\n"
        "    catch\n"
        "      InvalidOperationException => n := n + 4;\n"
        "    end;\n"
        "    return n;\n"
        "  end Plain;\n"
        "  public static async method Tick(): Task<int>\n"
        "    return 1;\n"
        "  end Tick;\n"
        "  public static method Main(): int\n"
        "    return C.Plain();\n"
        "  end Main;\n"
        "end C;\n"), 5);
}
