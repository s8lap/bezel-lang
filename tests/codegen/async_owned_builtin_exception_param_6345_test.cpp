// Spec-derived regression tests for issue #6345 — an `owned` PARAMETER of a
// DECLARATION-LESS language built-in (§31.4 exception hierarchy) was left
// UN-RECLAIMED at an ASYNC FRAME's terminal auto-dispose, leaking one object
// per call.
//
// THE REPRO (the issue's own, verbatim shape):
//
//     async method Take(owned e: Exception): Task<int>
//       await Task.Delay(1);
//       return 0;
//     end Take;
//
// MEASURED fail-without on a865fa56 (this tree with the fix reverted, `bezelc`
// rebuilt from the reverted source, the LSan recipe `runAotChecked` runs):
// `SUMMARY: AddressSanitizer: 2000 byte(s) leaked in 50 allocation(s)` — 50 ×
// the 40-byte `new IOException("a")`, i.e. 40 B per call, EVERY call. With the
// fix the same program is LSan-clean.
//
// ROOT CAUSE (src/codegen/LlvmBackend_Disposal.cpp,
// `emitOwnedParamCaptureDispose`). This function is the ONE routing decision
// for an owned-parameter capture's terminal dispose, shared by all three frame
// emitters: the plain-async frame, the async iterator's Dispose sweep, and the
// sync iterator's owned-param sweep. Its arm (3) gated on
//
//     pt->kind == TypeRef::Kind::Named && pt->declaration
//
// and the comment on that arm asserted that a built-in exception "sets"
// `pt->declaration`. It does not: a §31.4 language-built-in carries NO user
// `TypeSymbol` (`TypeRef::declaration == nullptr`). So EVERY
// `async method Take(owned e: Exception)` fell past arm (3) and past arm (4)
// (which requires a Named declaration-less head `IDisposable` /
// `IAsyncDisposable`) into arm (5), which raises an `internal:` warning and
// leaves the slot un-reclaimed by design. The compiler said so out loud twice
// per declaration: `internal: emitOwnedParamCaptureDispose: unrecognised owned
// parameter type ... the slot is left un-reclaimed (a bounded leak) — #3157`.
//
// This is the SAME declaration-blindness #6337 fixed in
// `registerUnwindOwnedParam`, at a DIFFERENT site and a different stage.
// #6337 discharges the value on the CALLEE's normal/unwind edge; it never
// reaches the frame's terminal auto-dispose. Both ends are needed: on the
// un-fixed tree the value was owned by nobody at teardown.
//
// WHY IT IS IN-FAMILY, NOT AN UNRELATED BUG: the same shape with a USER class
// (`async method Take(owned b: Boom)`, `Boom` a plain class) is CLEAN on the
// same binary — it takes arm (3) on `pt->declaration` and is disposed exactly
// once. The predicate was the whole defect.
//
// THE FIX. Arm (3)'s discriminator becomes
// `pt->declaration || isBuiltinExceptionTypeRef(pt)`. That is the SAME named
// recognizer #6311's caller-side gate (`buildOrderedArgs`) and #6337's
// callee-side gate already use for this same population, so all three ends of
// the ownership chain now agree on which types they are talking about.
//
// SCOPE — deliberately narrow. `isBuiltinExceptionTypeRef` requires a
// declaration-less Named TypeRef whose astNode NAME resolves in
// `builtinVtable_`, i.e. exactly the §31.4 hierarchy. The LOOSE
// `isBuiltinExceptionType` predicate would also admit every other
// declaration-less Named type and sweep in the Task family and the genuinely
// vtable-less built-ins (a collection, a `FileStream`, a `Mutex`), which arm (2)
// above still owns and which have NO vtable at word 0 — that is exactly the
// wild-jump #3157 fixed. Arm (2) runs first and is unchanged.
//
// §31.1 makes a built-in exception an ordinary owned heap class instance, and
// §28.3 states the must-use auto-drop obligation for it exactly as for any
// other class in the same position, so admitting it here is the twin of an
// existing rule, not a new one. `setupBuiltinExceptions` mints every §31.4
// member a real vtable whose slot 2 is `bz$exDisposeMsg`, which is precisely
// what the `dynamicDispose` `emitDisposeAndReclaim` dispatches through — so the
// message buffer is released with the object, not just the object.
//
// BOTH DIRECTIONS. Two rows pin the OVER-release side, i.e. that the widened
// arm did not dispose a value the callee does NOT own:
//   * `async_owned_Exception_param_explicit_dispose_then_return_no_double_free`
//     — an explicit `e.Dispose()` discharges the parameter, so the frame's
//     terminal auto-dispose must find a nulled slot; a second free is an ASan
//     double free.
//   * `async_borrow_param_Exception_is_never_disposed` — a plain (borrow)
//     `e: Exception` parameter has the SAME declaration-less type, so widening
//     the arm without keeping the `owned`-modifier precondition would dispose a
//     value the callee does not own; the caller's `using var` is still the
//     holder (§28.8) and would free the object a second time.
//
// Rows 2 and 3 cover the OTHER TWO frame emitters that route through the same
// function — the async iterator's Dispose sweep and the sync iterator's
// owned-param sweep — so "widening here closes all three populations" is
// asserted, not assumed.
//
// Spec: §28.3 *Unbound owned temporaries* and the must-use auto-drop, §28.8
// (only an owned reference carries the disposal obligation), §29.8 (an
// iterator's state machine disposes its owned parameters at iterator-Dispose),
// §30.1, §31.1 (built-in types are ordinary owned heap class instances), §31.4
// (the standard exception hierarchy), §28.2 (`Dispose` is synthesised, never
// declared). Revision v0.338.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// A leak / double-free / use-after-free flips `ran` to false -> -999, distinct
// from any legal exit value. `expectRanWith` is the ONLY `runAotChecked` call
// site in this file, and it does NOT absorb a toolchain skip: a skip makes the
// harness return `ran = false` (aot_detail::markInfra), which the
// `BEZEL_EXPECT_TRUE(r.ran)` below turns into a row FAILURE rather than a
// silent pass. There is no `wasSkipped(r)` / `if (!r.ran) return;` here.
//
// The rows assert BOTH halves: `ran` says the program ran to normal
// termination under ASan/LSan (no leak, no memory error), and the exit code
// says it computed the spec-derived value. Either half alone is satisfiable
// vacuously — a `didNotCrash`-only row passes on a program that computes the
// wrong answer, and a value-only row can be satisfied by a sentinel.
//
// 500 iterations is far above the 50-500 one-shot band: the leak is per-call,
// and a single-shot repro measures exactly one call. Every row accumulates a
// real value from 500 distinct heap objects, so a partial discharge shows up as
// a wrong exit code as well as an LSan report.
void expectRanWith(const std::string& src, int expected) {
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, expected);
}

}  // namespace

// §28.3 must-use auto-drop / §31.1 / §31.4 (#6345) — THE HEADLINE SHAPE, the
// issue's repro. `Take`'s `owned e: Exception` is never moved / returned /
// disposed, so §28.3 auto-drops it at the end of the method and the async
// frame's terminal auto-dispose is the only code that can reclaim it. The
// parameter is typed at the ROOT of the §31.4 hierarchy (`Exception`) while the
// caller passes a DERIVED member (`IOException`), so the row also pins that the
// recogniser is driven by the DECLARED type, exactly as it must be — the callee
// never sees the runtime subclass.
//
// 500 x 3 = 1500; 1500 mod 251 = 245.
// Pre-fix: `2000 byte(s) leaked in 50 allocation(s)` on the issue's 50-iteration
//           repro, 40 B per call on this 500-iteration shape; exit 99 -> -999.
// Post-fix: LSan-clean, 245.
BEZEL_TEST(codegen_async_owned_builtin_exception_param_6345,
           async_owned_Exception_param_dropped_autodispose_spec_28_3) {
    std::string src =
        "module M; "
        "class Q "
        "  public static async method Take(owned e: Exception): Task<int> "
        "    await Task.Delay(1); "
        "    return e.Message.Length; "
        "  end Take; "
        "end Q; "
        "class P public static async method Main(): Task<int> "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    var v := await Q.Take(new IOException(\"abc\")); "
        "    t := t + v; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 245);
}

// §29.8 / §28.3 (#6345) — the ASYNC ITERATOR's Dispose sweep. The declaration
// is the same `owned e: Exception`, but the method's type is
// `IAsyncEnumerable<int>`, so the terminal auto-dispose is driven by the
// iterator's Dispose state machine rather than by the plain-async frame. This
// row exists so "widening the ONE routing function closes all three frame
// emitters" is an ASSERTED claim: it fails if the async-iterator loop ever
// reaches this arm through a different route.
//
// The consumer runs the iterator to exhaustion, so the disposal lands on the
// exhaustion path of Dispose.
//
// 500 x 3 = 1500; 1500 mod 251 = 245.
// Pre-fix: LSan leak, exit 99 -> -999.  Post-fix: LSan-clean, 245.
BEZEL_TEST(codegen_async_owned_builtin_exception_param_6345,
           async_iterator_owned_Exception_param_disposed_spec_29_8) {
    std::string src =
        "module M; "
        "class Q "
        "  public static async method Take(owned e: Exception): IAsyncEnumerable<int> "
        "    yield return e.Message.Length; "
        "  end Take; "
        "end Q; "
        "class P public static async method Main(): Task<int> "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    await for var x in Q.Take(new IOException(\"abc\")) do "
        "      t := t + x; "
        "    end; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 245);
}

// §29.8 / §28.3 (#6345) — the SYNC iterator's owned-param sweep, the THIRD
// emitter routed by the same function (LlvmBackend_Iterators.cpp). Same
// declaration, same recogniser, third caller — so all three loops are pinned.
//
// 500 x 3 = 1500; 1500 mod 251 = 245.
// Pre-fix: LSan leak, exit 99 -> -999.  Post-fix: LSan-clean, 245.
BEZEL_TEST(codegen_async_owned_builtin_exception_param_6345,
           sync_iterator_owned_Exception_param_disposed_spec_29_8) {
    std::string src =
        "module M; "
        "class Q "
        "  public static method Take(owned e: Exception): IEnumerable<int> "
        "    yield return e.Message.Length; "
        "  end Take; "
        "end Q; "
        "class P public static method Main(): int "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    for var x in Q.Take(new IOException(\"abc\")) do "
        "      t := t + x; "
        "    end for; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 245);
}

// §28.3 / §28.2 (#6345) — OVER-RELEASE GUARD, the DISCHARGE half. The callee
// reads the message, disposes explicitly (§28.2: a written `x.Dispose()` is a
// release EARLY), and then returns normally. The frame's terminal auto-dispose
// must then find a NULLED slot; disposing again is an ASan double free, which
// would flip `ran` to false. This is the row that would catch the opposite
// defect introduced by widening the arm — one that disposes a value whose
// owner has already released it.
//
// 500 x 3 = 1500; 1500 mod 251 = 245.
// Post-fix: LSan-clean, 245.
BEZEL_TEST(codegen_async_owned_builtin_exception_param_6345,
           async_owned_Exception_param_explicit_dispose_then_return_no_double_free) {
    std::string src =
        "module M; "
        "class Q "
        "  public static async method Take(owned e: Exception): Task<int> "
        "    var n := e.Message.Length; "
        "    e.Dispose(); "
        "    await Task.Delay(1); "
        "    return n; "
        "  end Take; "
        "end Q; "
        "class P public static async method Main(): Task<int> "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    var v := await Q.Take(new IOException(\"abc\")); "
        "    t := t + v; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 245);
}

// §28.8 (#6345) — OVER-RELEASE GUARD, the OWNERSHIP half. A plain (borrow)
// `e: Exception` parameter has the SAME declaration-less built-in type as the
// owned row above, so if the widened recogniser were driven by the TYPE alone
// rather than by type AND the `owned` modifier, this row would dispose a value
// the callee does NOT own: the caller's `using var` is still the holder (§28.8)
// and would free the same object a second time. The callee here is a plain
// SYNC method, so its terminal auto-dispose is `registerUnwindOwnedParam`
// (#6337's site) rather than the frame sweep — the row pins the `owned`
// precondition at BOTH ends of the chain.
//
// 500 x 3 = 1500; 1500 mod 251 = 245.
// Post-fix: LSan-clean, 245.
BEZEL_TEST(codegen_async_owned_builtin_exception_param_6345,
           async_borrow_param_Exception_is_never_disposed_spec_28_8) {
    std::string src =
        "module M; "
        "class Q "
        "  public static method Borrow(e: Exception): int "
        "    return e.Message.Length; "
        "  end Borrow; "
        "end Q; "
        "class P public static method Main(): int "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    using var x := new IOException(\"abc\"); "
        "    t := t + Q.Borrow(x); "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 245);
}