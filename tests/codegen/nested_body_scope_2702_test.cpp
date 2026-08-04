// Codegen — issue #2702: the lazily-emitted, MEMOIZED block-bodied property
// getter leaked more of the CALLER's emission state than #2170/#2274/#2694
// closed. `smStateGEP_` was the confirmed member: a block getter whose body
// contains a `try` and whose first reader sits inside an `async` method emitted
// a store into an instruction belonging to the CALLER's function, and the
// module failed IR verification.
//
// Spec (revision v0.240):
//   §23.3 "Block-form properties" — "Each accessor uses `=> expression` or
//     `=> do ... end`." A `do ... end` accessor is a user statement body lowered
//     into its OWN function; its `return` terminates the accessor, so the
//     accessor carries its own emission contract and inherits none of the
//     reader's.
//   §30.5 "Result lifetime and Task disposal" — an `async` method's return
//     completes the outer task. That completion pathway belongs to the async
//     method and must not fire inside a non-async helper function reached from
//     its body.
//   §31.2 "try/catch/finally" — "`finally` runs whether the protected block
//     completed normally, was exited by an exception (caught or uncaught), or
//     was exited by `return` — or by a `break` or `continue` targeting an
//     enclosing loop." The getter's own `try` must lower against the getter's
//     frame, not the caller's suspend frame.
//
// THE BUG. `ensurePropertyBlockGetter` (LlvmBackend_Assign.cpp) switches
// `currentFn_` into a standalone function but inherits the caller's other
// emission state. #2170 added `smKind_`/`smStateStruct_`; #2274/#2281 added
// `finallyStack_`/`finallyBodyStack_`; #2694 added `currentReturnTypeNode_`/
// `currentLambdaReturnSemaTy_`. `smStateGEP_` — the pointer to the CALLER's
// state-machine state slot — was still inherited, and `lowerTry`
// (LlvmBackend_Stmts.cpp) reads it on two paths:
//   * the unwind edge stores `-1` through it gated on `smStateGEP_` ALONE, so
//     #2170's nulling of `smStateStruct_` does not protect it; and
//   * a second path is gated on `smStateGEP_ && (smKind_ == Yield ||
//     smKind_ == AsyncYield)` — and #2170's own reset value for `smKind_` is
//     `Yield` (the enum has no neutral member), so that gate is OPEN inside
//     every block getter.
// Observed pre-fix on the repro below:
//     Referring to an instruction in another function!
//       store i32 -1, ptr %4, align 4
//     Function return type does not match operand type of return inst!
//       ret i1 false
//      i32
//     bezelc: codegen error: IR verification failed
//
// THE FIX. `Emitter::NestedBodyScope` — one RAII guard that saves, neutralises
// and restores the whole per-body emission set (the suspend/resume frame
// including `smStateGEP_`, the iterator and async frame fields, the
// destructor-body pair `emittingDisposerBody_`/`disposerEntryExc_`, the
// catch-arm pair, the narrow per-call channels, and #2694's return-contract
// pair), so the invariant holds by construction instead of depending on each
// new field being remembered at three separate call sites. Members that are
// CORRECT to inherit (`genericSubst_`, `genericSubstSemaTy_`,
// `currentEnumMethod_`, `currentSpecMethodSymbol_`, the template-body flags)
// are listed explicitly on the guard with the reason each is left alone.
//
// ⚠ WHY THE FAILURE IS A COMPILE FAILURE, NOT A WRONG VALUE. The defect emits
// malformed IR that `verifyModule` rejects, so the AOT compile aborts and
// `runAot` reports ran=false. The harness turns that into the -999 sentinel,
// which is what these cases assert against pre-fix.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

bool wasSkipped(const JitResult& r) {
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.message.find("SKIP") != std::string::npos) return true;
    }
    return false;
}

// Plain (non-ASan) AOT run: the exit code alone, or -999 when the compile or
// the run did not complete. `--full` has masked live defects before (#2633 /
// #2629), so every case below is checked on the plain oracle too.
int runPlain(const std::string& src) {
    JitResult r = bezel::test::runAot(src);
    return r.ran ? r.exitCode : -999;
}

int runChecked(const std::string& src) {
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return -1000;   // ASan toolchain absent — case skipped
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// (1) THE CONFIRMED REPRO (#2702 item 1, `smStateGEP_`). A block-bodied `int`
// property whose body wraps its `return` in a `try … finally`, first (and only)
// read from inside an `async` method. Pre-fix: IR verification failure, so the
// program does not run at all (-999). Post-fix: 7 flows out through the getter,
// the async result is 7, and Main returns 7 - 7 + 3 = 3.
// §23.3 (block accessor) + §31.2 (finally) + §30.5 (async return).
static const char* kTryInGetterFromAsync =
    "module M; "
    "class Owner "
    "  private _n: int; "
    "  public constructor() _n := 7; end constructor; "
    "  public property N: int "
    "    get => do "
    "      try "
    "        return _n; "
    "      finally "
    "        var z := 1; "
    "      end; "
    "    end; "
    "  end N; "
    "end Owner; "
    "class P "
    "  public static async method RunAsync(): Task<int> "
    "    using var o := new Owner(); "
    "    return o.N; "
    "  end RunAsync; "
    "  public static async method Main(): Task<int> "
    "    using var t := P.RunAsync(); "
    "    var r := await t; "
    "    return r - 7 + 3; "          // 3
    "  end Main; "
    "end P;";

BEZEL_TEST(codegen_nested_body_scope_2702, try_in_block_getter_read_from_async) {
    BEZEL_EXPECT_EQ(runPlain(kTryInGetterFromAsync), 3);
}

BEZEL_TEST(codegen_nested_body_scope_2702,
           try_in_block_getter_read_from_async_checked) {
    int rc = runChecked(kTryInGetterFromAsync);
    if (rc == -1000) return;   // ASan toolchain absent
    BEZEL_EXPECT_EQ(rc, 3);
}

// (2) The `finally` body must still RUN inside the getter — the fix must not
// have neutralised the getter's own try machinery along with the caller's
// frame. The finally increments a counter that is folded into the value the
// getter returns, so a skipped finally is observable as a wrong exit code
// rather than as a crash. §31.2.
// Getter: _n starts 7; finally sets _k := 5; the getter returns _n and Main
// reads K afterwards, so the result is 7 + 5 = 12.
static const char* kFinallyRunsInsideGetter =
    "module M; "
    "class Owner "
    "  private _n: int; "
    "  private _k: int; "
    "  public constructor() _n := 7; _k := 0; end constructor; "
    "  public property N: int "
    "    get => do "
    "      try "
    "        return _n; "
    "      finally "
    "        _k := 5; "
    "      end; "
    "    end; "
    "  end N; "
    "  public property K: int "
    "    get => _k; "
    "  end K; "
    "end Owner; "
    "class P "
    "  public static async method RunAsync(): Task<int> "
    "    using var o := new Owner(); "
    "    var a := o.N; "
    "    return a + o.K; "           // 7 + 5 = 12 only if the finally ran
    "  end RunAsync; "
    "  public static async method Main(): Task<int> "
    "    using var t := P.RunAsync(); "
    "    var r := await t; "
    "    return r; "                 // 12
    "  end Main; "
    "end P;";

BEZEL_TEST(codegen_nested_body_scope_2702, finally_still_runs_inside_block_getter) {
    BEZEL_EXPECT_EQ(runPlain(kFinallyRunsInsideGetter), 12);
}

// (3) The INDEXER spelling of (1) is deliberately NOT here. This commit gives
// the guard to `ensurePropertyBlockGetter` only; `ensureIndexerBlockGetter`
// adopts it in the #2701 commit, where the indexer cases live
// (tests/codegen/indexer_block_getter_async_2701_test.cpp). Splitting them that
// way keeps a bisect sharp: each commit's own test fails without it.

// (4) The DESTRUCTOR half (#2702 item 2, `emittingDisposerBody_` /
// `disposerEntryExc_`). Confirmed by instrumentation that a block getter whose
// FIRST toucher is a `destructor` body inherits both — `disposerEntryExc_` is a
// load in the destructor's OWN entry block, and `emitPendingCheck` compares
// against it after any call in the lowered body. The getter body below contains
// a call, which is the shape that reaches that comparison.
// The destructor folds the getter's value into a static so the result is
// observable: Owner.N is 3 + 4 = 7, so Main returns 7 - 7 + 5 = 5.
// §23.3 + §28.4 (destructor bodies).
static const char* kGetterFirstTouchedFromDestructor =
    "module M; "
    "class Leaf "
    "  private _v: int; "
    "  public constructor() _v := 3; end constructor; "
    "  public method Get(): int "
    "    return _v; "
    "  end Get; "
    "end Leaf; "
    "class Owner "
    "  private owned _leaf: Leaf; "
    "  public constructor() _leaf := new Leaf(); end constructor; "
    "  public property N: int "
    "    get => do "
    "      var a := _leaf.Get(); "
    "      return a + 4; "            // 7
    "    end; "
    "  end N; "
    "end Owner; "
    "class Holder "
    "  private owned _o: Owner; "
    "  public constructor() _o := new Owner(); end constructor; "
    "  destructor "
    "    Sink.Seen := _o.N; "
    "  end destructor; "
    "end Holder; "
    "class Sink "
    "  public static Seen: int := 0; "
    "end Sink; "
    "class P "
    "  public static method Main(): int "
    "    using var h := new Holder(); "
    "    h.Dispose(); "
    "    return Sink.Seen - 7 + 5; "  // 5
    "  end Main; "
    "end P;";

BEZEL_TEST(codegen_nested_body_scope_2702, block_getter_first_touched_from_destructor) {
    BEZEL_EXPECT_EQ(runPlain(kGetterFirstTouchedFromDestructor), 5);
}

BEZEL_TEST(codegen_nested_body_scope_2702,
           block_getter_first_touched_from_destructor_checked) {
    int rc = runChecked(kGetterFirstTouchedFromDestructor);
    if (rc == -1000) return;
    BEZEL_EXPECT_EQ(rc, 5);
}

// (5) OVER-CORRECTION CONTROL — the guard must NOT reset what it inherits
// correctly. A block-bodied getter on a GENERIC class resolves its own return
// type through `genericSubst_`/`genericSubstSemaTy_`, which the guard
// deliberately leaves alone (the getter's SIGNATURE is built under the caller's
// substitution before the guard runs, so clearing the map would lower the body
// against a different substitution than its signature). If a future edit adds
// either map to the reset list, this case stops compiling or returns the wrong
// value. §14.2 (constraints/instantiation) + §23.3.
//
// ⚠ THE FIELD AND THE PROPERTY MUST BOTH BE `T`, NOT `int`. An earlier version
// of this case declared `_v: int` / `V: int`, so `T` appeared only in the class
// header and at the instantiation site and NOTHING in the getter resolved a
// type parameter — it could not distinguish "map inherited" from "map cleared"
// and was a plain smoke test wearing a control's name. Review of #2702 caught
// that. Keep `T` in the getter's own signature and body, or this case silently
// stops guarding the inherit decision.
// Box<int>.V returns 21; Main returns 21 - 18 = 3.
static const char* kGenericBlockGetterControl =
    "module M; "
    "class Box<T> "
    "  private _v: T; "
    "  public constructor(v: T) _v := v; end constructor; "
    "  public property V: T "
    "    get => do "
    "      return _v; "
    "    end; "
    "  end V; "
    "end Box; "
    "class P "
    "  public static method Main(): int "
    "    using var b := new Box<int>(21); "
    "    return b.V - 18; "           // 3
    "  end Main; "
    "end P;";

BEZEL_TEST(codegen_nested_body_scope_2702, generic_block_getter_control) {
    BEZEL_EXPECT_EQ(runPlain(kGenericBlockGetterControl), 3);
}

// (6) OVER-CORRECTION CONTROL — an ordinary block getter read from an ordinary
// (non-async, non-destructor) method still works. This is the case that would
// break if the guard reset a field the plain path depends on. §23.3.
static const char* kPlainBlockGetterControl =
    "module M; "
    "class Owner "
    "  private _n: int; "
    "  public constructor() _n := 7; end constructor; "
    "  public property N: int "
    "    get => do "
    "      return _n; "
    "    end; "
    "  end N; "
    "end Owner; "
    "class P "
    "  public static method Main(): int "
    "    using var o := new Owner(); "
    "    return o.N - 4; "            // 3
    "  end Main; "
    "end P;";

BEZEL_TEST(codegen_nested_body_scope_2702, plain_block_getter_control) {
    BEZEL_EXPECT_EQ(runPlain(kPlainBlockGetterControl), 3);
}
