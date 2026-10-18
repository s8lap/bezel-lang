// Codegen — §28.3 / §28.8 / §14.1 / §29.8 / §30.1, v0.338 (#6367): the
// GENERIC spelling of the async `owned`-parameter frame discharge.
//
// An `async` method's `owned` parameters transfer into the state-machine frame
// and are reclaimed by the frame at the terminal state (§30.1). That reclamation
// is ONE routing decision — `emitOwnedParamCaptureDispose` — and it reads the
// DECLARED parameter type. For a generic method `async method Take<T>(owned e:
// T)`, the declared type is a bare type-parameter `TypeRef`: `declaration ==
// nullptr`, head name `T`. So arm (3)'s `pt->declaration` test misses it and
// `isBuiltinExceptionTypeRef(pt)` — which resolves the astNode NAME against
// `builtinVtable_` — misses it too, and arm (5) fires: a loud `internal:`
// warning plus a bounded leak.
//
// #6337 fixed exactly this for the CALLEE end
// (`registerUnwindOwnedParam`), so the two ends disagreed for the generic
// spelling: the callee substituted, the frame did not. This file's rows pin the
// frame end.
//
// MEASURED on 85a3cb36 (base), 500 iterations each:
//
//   Take<Exception>(new IOException("abc")) via await   20000 B / 500 allocs
//   Take<Exception>(new IOException("abc")) via .Wait() 20000 B / 500 allocs
//   Take<Boom>(new Boom())                              8000 B / 500 allocs
//   async-iterator Take<Exception>(...)                 20000 B / 500 allocs
//
// and in every one the `internal:` arm-(5) warning is raised FOUR times per
// declaration — the success edge, the fault edge, and the two state-machine
// emission paths that reach the routing decision. That 4x is not cosmetic: it
// is the measure of how many code paths needed the same one-line answer.
//
// Fix: resolve the declared type through the active generic substitution
// (`resolveTypeParamRef`, the SAME function `registerUnwindOwnedParam` already
// applies at this population for this same reason) before any arm reads it.
//
// Spec: §28.3 (must-use auto-drop of an unbound owned temporary — the frame's
// terminal auto-dispose is the only code that can reclaim the value),
// §28.8 (only an owned reference carries the disposal obligation), §14.1
// (type parameters and their substitution), §29.8 (an iterator's state machine
// disposes its owned parameters at iterator-Dispose), §30.1 (an async method's
// owned parameters are captured by the frame). Revision v0.338.

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
// vacuously.
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

// §28.3 / §14.1 (#6367) — THE HEADLINE SHAPE, the issue's own spelling. `Take`'s
// `owned e: T` is never moved / returned / disposed, so §28.3 auto-drops it at
// the end of the method and the frame's terminal auto-dispose is the only code
// that can reclaim it. The row drives the task with `Wait()` + `Result`
// (§30.3's synchronous escape hatches) rather than `await`, so the discharge is
// observed on the plain frame path with no second state machine in play.
//
// `T` binds to the §31.4 hierarchy ROOT (`Exception`) while the caller passes a
// DERIVED member (`IOException`), so the row also pins that the recogniser is
// driven by the SUBSTITUTED DECLARED type — the callee never sees the runtime
// subclass.
//
// 500 x 1 = 500; 500 mod 251 = 249.
// Pre-fix: `SUMMARY: AddressSanitizer: 20000 byte(s) leaked in 500 allocation(s)`
//           plus 4 x the arm-(5) `internal:` warning; exit 99 -> -999.
// Post-fix: LSan-clean, 249.
BEZEL_TEST(codegen_async_owned_generic_param_frame_discharge_6367,
           generic_owned_Exception_param_dropped_autodispose_spec_28_3) {
    std::string src =
        "module M; "
        "class Q "
        "  public static async method Take<T>(owned e: T): Task<int> "
        "    await Task.Delay(1); "
        "    return 1; "
        "  end Take; "
        "end Q; "
        "class P public static method Main(): int "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    var task := Q.Take<Exception>(new IOException(\"abc\")); "
        "    task.Wait(); "
        "    t := t + task.Result; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 249);
}

// §30.1 / §28.3 (#6367) — the same declaration driven by `await`, so the
// discharge is observed on the awaited-completion edge of the frame rather than
// on the synchronous one. The two spellings reach the routing decision through
// different state-machine states, and the arm-(5) warning's 4-per-declaration
// count is the measure that several do; these two rows are the halves that keep
// "one substitution closes the population" honest.
//
// 500 x 1 = 500; 500 mod 251 = 249.
// Pre-fix: 20000 B / 500 allocs.  Post-fix: LSan-clean, 249.
BEZEL_TEST(codegen_async_owned_generic_param_frame_discharge_6367,
           generic_owned_Exception_param_awaited_frame_edge_spec_30_1) {
    std::string src =
        "module M; "
        "class Q "
        "  public static async method Take<T>(owned e: T): Task<int> "
        "    await Task.Delay(1); "
        "    return 1; "
        "  end Take; "
        "end Q; "
        "class P public static async method Main(): Task<int> "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    var v := await Q.Take<Exception>(new IOException(\"abc\")); "
        "    t := t + v; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 249);
}

// §28.3 / §14.1 (#6367) — the USER-CLASS substitution, which is a different arm
// than the headline row's and therefore the row that shows the substitution
// reaches arm (3) as well as arm (3)'s builtin-exception recogniser. `Boom` has
// a real `declaration` once substituted, so after the fix arm (3) dispatches
// through the ordinary user-class release.
//
// This is the shape where arm (3)'s `pt->declaration` test is the one that was
// silently false: pre-fix, `pt` was the type-parameter ref, so the test read
// `nullptr` and the class arm was never reached.
//
// 500 x 1 = 500; 500 mod 251 = 249.
// Pre-fix: `SUMMARY: AddressSanitizer: 8000 byte(s) leaked in 500 allocation(s)`.
// Post-fix: LSan-clean, 249.
BEZEL_TEST(codegen_async_owned_generic_param_frame_discharge_6367,
           generic_owned_user_class_param_dropped_autodispose_spec_28_3) {
    std::string src =
        "module M; "
        "class Boom "
        "end Boom; "
        "class Q "
        "  public static async method Take<T>(owned e: T): Task<int> "
        "    await Task.Delay(1); "
        "    return 1; "
        "  end Take; "
        "end Q; "
        "class P public static async method Main(): Task<int> "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    var v := await Q.Take<Boom>(new Boom()); "
        "    t := t + v; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 249);
}

// §29.8 (#6367) — the ASYNC ITERATOR's Dispose sweep, the SECOND of the three
// frame emitters routed by the same function
// (`emitAsyncOwnedParamAutoDispose` and the sync-iterator sweep are the other
// two). The method's type is `IAsyncEnumerable<int>`, so the terminal
// auto-dispose is driven by the iterator's Dispose state machine rather than by
// the plain-async frame. The generic parameter is what #6337's callee twin
// already covered; this row is what makes the FRAME twin cover the same
// population.
//
// 500 x 1 = 500; 500 mod 251 = 249.
// Pre-fix: 20000 B / 500 allocs.  Post-fix: LSan-clean, 249.
BEZEL_TEST(codegen_async_owned_generic_param_frame_discharge_6367,
           async_iterator_generic_owned_param_disposed_spec_29_8) {
    std::string src =
        "module M; "
        "class Q "
        "  public static async method Take<T>(owned e: T): IAsyncEnumerable<int> "
        "    yield return 1; "
        "  end Take; "
        "end Q; "
        "class P public static async method Main(): Task<int> "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    await for var x in Q.Take<Exception>(new IOException(\"abc\")) do "
        "      t := t + x; "
        "    end; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 249);
}

// §28.3 / §14.1 (#6367) — THE OVER-RELEASE GUARD, and the row the fix's own
// safety argument rests on. `T` binds to a VALUE type here, so
// `resolveTypeParamRef` returns a PRIMITIVE `TypeRef`, not a Named one. Every
// arm of the routing decision is gated on `TypeRef::Kind::Named` or on a builtin
// NAME, so a primitive substitution falls through all of them and is left
// un-reclaimed — which is correct: an `int` owns nothing, and a release here
// would be a free of a non-heap value.
//
// This row is green before the fix and must STAY green. It is the row that
// makes the substitution's blast radius a measured property rather than an
// argument in a comment: if a future change lets a primitive substitution reach
// a release arm, this row's 500 calls turn a non-heap value into a free and LSan
// (or a SEGV) flips `ran`.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_async_owned_generic_param_frame_discharge_6367,
           generic_owned_param_value_substitution_is_never_freed) {
    std::string src =
        "module M; "
        "class Q "
        "  public static async method Take<T>(owned e: T): Task<int> "
        "    await Task.Delay(1); "
        "    return 1; "
        "  end Take; "
        "end Q; "
        "class P public static async method Main(): Task<int> "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    var v := await Q.Take<int>(5); "
        "    t := t + v; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 249);
}

// §28.3 / §28.2 / §14.2 (#6367) — the DOUBLE-FREE guard. An explicit
// `e.Dispose()` discharges the parameter, so the frame's terminal auto-dispose
// must find a nulled slot; a second release is an ASan double free.
//
// The `where T: IDisposable` constraint is what makes this spelling exist at
// all: on an UNCONSTRAINED `T` the callee has no `Dispose` member and the call
// is rejected outright (`no member 'Dispose' on unconstrained type parameter
// 'T'`), which is why the adoption spelling (`using var _ := owned e`) is the
// only other discharge — and that one is broken independently of this fix
// (#6392: a SYNC generic `Take<T>(owned e: T)` with that body leaks
// 19,920 B / 498 allocs at 500 iterations, while the non-generic
// `owned e: Exception` twin is clean; it is the `registerUsingDisposal` path,
// outside this fix's site).
//
// Honest caveat, MEASURED: at this spelling the compiler emits `internal:
// codegen cannot dispatch member 'Dispose' on receiver of type 'T': permissive
// acceptance (spec #3 / issue #605) admits this site but no concrete member
// dispatch exists (#2477)`. So the row asserts what it can — exactly one
// release of the object, no leak and no double free — and does NOT claim the
// explicit `Dispose()` call itself reached a concrete dispatch.
//
// Green before the fix (arm (5) declined everything, including the discharged
// slot) and green after it — which is the point: the fix ADDS a release for the
// slot when it is still live, and must not add one when it is not.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_async_owned_generic_param_frame_discharge_6367,
           generic_owned_param_explicit_dispose_then_return_no_double_free) {
    std::string src =
        "module M; "
        "class Q "
        "  public static async method Take<T>(owned e: T): Task<int> "
        "    where T: IDisposable "
        "    e.Dispose(); "
        "    await Task.Delay(1); "
        "    return 1; "
        "  end Take; "
        "end Q; "
        "class P public static async method Main(): Task<int> "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    var v := await Q.Take<Exception>(new IOException(\"abc\")); "
        "    t := t + v; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 249);
}

// §28.8 (#6367) — the OWNERSHIP guard, and the mirror of the #6345 rows. A
// plain (borrow) `e: T` parameter has the SAME declaration-less type as the
// owned one, so substituting without keeping the `owned`-modifier precondition
// would discharge a value the callee does not own; the caller's `using var` is
// still the holder (§28.8) and would free the object a second time.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_async_owned_generic_param_frame_discharge_6367,
           generic_borrow_param_is_never_disposed_spec_28_8) {
    std::string src =
        "module M; "
        "class Q "
        "  public static async method Peek<T>(e: T): Task<int> "
        "    await Task.Delay(1); "
        "    return 1; "
        "  end Peek; "
        "end Q; "
        "class P public static async method Main(): Task<int> "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    using var x := new IOException(\"abc\"); "
        "    var v := await Q.Peek<Exception>(x); "
        "    t := t + v; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 249);
}