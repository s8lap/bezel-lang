// Codegen / refcounting — issues #5300 and #5301, one root.
//
// #5300: the generic identity `Mk<T>(x: T): T` at `T = string` LEAKS when the
//        argument is a borrowed named local. MEASURED on the lane base
//        r23-c28 2c35b57f with `scripts/agent_check.sh --probe --asan`:
//        `SUMMARY: AddressSanitizer: 7253 byte(s) leaked in 398 allocation(s)`
//        at 200 iterations, every line printing correctly first.
// #5301: the same call USES FREED MEMORY when the argument is a fresh `+`
//        temporary. MEASURED on the same base:
//        `SUMMARY: AddressSanitizer: heap-use-after-free ... in main`.
//
// THREE user rows, FIVE controls. MEASURED pre-fix on the lane base
// r23-c28 (28644650), `scripts/agent_check.sh --area codegen` under
// `BEZEL_AOT_FORCE_SANITIZE=address`: the three user rows fail
// (7253 B / 398 allocs leaked; heap-use-after-free in main; 14580 B / 800
// allocs leaked) and all five controls pass. (ASan's allocation accounting
// varies a little run to run — an earlier leg of the same three rows reported
// 7290/400 and 14543/798; the finding, the rows and the direction do not
// change.) The third user row (`two_params_returned_second_is_source`) was
// carried in the inherited WIP as a "control" — MEASURED, it fails pre-fix
// with #5300's own root, so it is a user row and is labelled as one.
//
// ONE ROOT: §28.3 *Bare-type-parameter returns* was not applied
// per-instantiation at a runtime-managed (string) substitution. Pre-fix the
// compiler read the `T`-typed parameter as CONSUMING, in two places at once:
//
//   caller  `StringEscapeWalker::walkCallArgs` nulled the callee MethodSymbol
//           for ANY method with generic parameters, so every argument passed
//           to one was classified an unconditional ESCAPE and the tracked
//           string local's own scope-end release never armed  -> #5300;
//   callee  the returned parameter was forwarded as the argument's own
//           pointer, with no copy, so the call-boundary release of a fresh
//           temporary freed the very value the bind then read  -> #5301.
//
// THE MODEL, MEASURED NOT ASSUMED. `p` is still readable — and prints
// correctly — after `var x := Mk<string>(p);`, so a plain (non-`owned`)
// type-parameter argument is a BORROW SOURCE, not a transfer (§28.3 clause 4:
// a plain parameter is a potential SOURCE, and "`owned` parameters are never
// sources — they are consumed into the callee and die there" applies only to
// `owned`). The result is therefore a COPY, exactly as §28.3's callee-side
// rule 1 states for a runtime-managed substitution: "returning a parameter
// COPIES the value". §28.3 caller clause 3 then adds the caller half: at a
// value or runtime-managed instantiation the result is not a borrow and no
// constraint attaches (§28.9) — the caller TRANSFERS the caller's reference
// to that fresh one.
//
// The gate is the call site's SUBSTITUTED expression type, not the syntactic
// `T`: at `T = SomeClass` the substitution is a value-shaped class, the
// result is a BORROW and mints no +1. So every site below is additionally
// keyed on the substituted result being a `string` primitive — which is why
// the `Mk<int>` / `Mk<long>` / `Mk<bool>` control row is untouched by the
// change and stays green in every leg.
//
// THE FIX. One pre-substitution predicate, `methodReturnsBareTypeParam`
// (a free function in LlvmBackend_impl.hpp, keyed exactly as §28.3's clause 1
// keys it: "read from the signature BEFORE generic substitution, with
// nullability stripped — `T?` qualifies; an `owned T` return is not bare"),
// drives all three coordinated sites:
//   1. the walker's generic-method exclusion (LlvmBackend_Setup.cpp) so a
//      plain parameter of such a method is admitted as a borrow source;
//   2. `isUserMethodStringReturnCall` (LlvmBackend_Setup.cpp) so the bind
//      recognises the callee as minting +1 and stops re-minting one;
//   3. the call site (LlvmBackend_Calls2.cpp) mints the one independent +1
//      for the result.
//
// Spec: §14.1 (generic instantiation and its site), §28.3 *Bare-type-parameter
// returns*, §28.9 *Runtime-managed types* — reference-counting implementation
// unspecified, so the contract is only dispose-exactly-once. Spec revision
// v0.313.
//
// runAotChecked = generated-code ASan + LeakSanitizer, fork-isolated
// Driver::compileFile. Every row builds its strings at RUNTIME (a literal is
// an rc sentinel that hides both bugs) and is loop-amplified 200x, and every
// assertion is on stdout, never on an exit code: a use-after-free reads freed
// memory and can exit 0.

#include <cstdio>
#include <iostream>
#include <string>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"   // runAotChecked
#include "support/build_paths.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// ── the shared driver ─────────────────────────────────────────────────────
// A sanitizer finding (a leak, a UAF) is reported by runAotChecked as an
// Error diagnostic, so `!hasErrors()` is the dispose-exactly-once claim; the
// stdout comparison is a SEPARATE, independent claim that the program also
// computed the right answer (a UAF can print the right value from freed
// memory, so the two must not be collapsed into one).
bool disposeExactlyOnce(const std::string& src, const std::string& want) {
    auto r = runAotChecked(src);
    if (r.diagnostics.hasErrors()) {
        std::cout << "    runAotChecked reported diagnostics (expected a clean "
                     "run):\n";
        for (const auto& d : r.diagnostics.diagnostics())
            std::cout << "      " << d.message << "\n";
        return false;
    }
    if (!r.ran) {
        std::cout << "    program did not run to completion\n";
        return false;
    }
    if (r.stdoutText != want) {
        std::cout << "    stdout was \"" << r.stdoutText << "\", expected \""
                  << want << "\"\n";
        return false;
    }
    return true;
}

// The generic identity under test, declared identically by every user row.
// `static` (module-private) is enough: the call sites are in the same module.
const char* kIdentity = "static method Mk<T>(x: T): T => x; ";

// ═══════════════════════════════════════════════════════════════════════════
// USER ROW 1 — #5300. A BORROWED named local passed to the identity. The
// local keeps its own reference after the call, so the call may only ADD one
// for the result; pre-fix the walker's blanket generic exclusion suppressed
// the local's scope-end release entirely and the whole loop leaked.
// ═══════════════════════════════════════════════════════════════════════════
std::string borrowedArgSrc(int iters) {
    return std::string("module M5300; "
                       "public class Program "
                       "  ") + kIdentity +
           "  public static method Main(): int "
           "    var t := 0; "
           "    for var i in 0..<" +
           std::to_string(iters) +
           " do "
           "      var p: string := \"w\" + i.ToString(); "
           "      var x := Mk<string>(p); "
           "      t := t + x.Length; "
           "    end; "
           "    Console.WriteLine(t); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

// ═══════════════════════════════════════════════════════════════════════════
// USER ROW 2 — #5301. A FRESH `+` temporary passed straight to the identity.
// The temporary owns its only reference; the result is a copy of it, so the
// call-boundary release must drop one refcount and the result's own +1 must
// keep the value alive for the bind. Pre-fix there was no copy, the boundary
// release freed the value, and the bind's read was a use-after-free.
// ═══════════════════════════════════════════════════════════════════════════
std::string freshArgSrc(int iters) {
    return std::string("module M5301; "
                       "public class Program "
                       "  ") + kIdentity +
           "  public static method Main(): int "
           "    var t := 0; "
           "    for var i in 0..<" +
           std::to_string(iters) +
           " do "
           "      var x := Mk<string>(\"w\" + i.ToString()); "
           "      t := t + x.Length; "
           "    end; "
           "    Console.WriteLine(t); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

// ═══════════════════════════════════════════════════════════════════════════
// CONTROLS. These must pass in EVERY leg — pre-fix and post-fix — so that a
// red user row is attributable to the change and not to the surrounding
// machinery.
// ═══════════════════════════════════════════════════════════════════════════

// (C1) The non-generic twin of user row 1: the same borrowed local, the same
// runtime-built string, no generic call at all. 690 = sum over i of
// (1 + digits(i)) for i in 0..<200 = 200 + (10*1 + 90*2 + 100*3).
std::string noGenericSrc(int iters) {
    return "module C5301; "
           "public class Program "
           "  public static method Main(): int "
           "    var t := 0; "
           "    for var i in 0..<" +
           std::to_string(iters) +
           " do "
           "      var p: string := \"w\" + i.ToString(); "
           "      t := t + p.Length; "
           "    end; "
           "    Console.WriteLine(t); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

// (C2) THE control the brief names: the same identity at the three
// non-string instantiations. §28.9 makes `string` the runtime-managed type;
// at a value substitution there is no refcount to mint or drop, and the
// fix's `string`-primitive gate is the only thing standing between this row
// and the change. 20300 = sum(i) 19900 + 200 (`Mk<long>` equal) + 200
// (`Mk<bool>` true).
std::string valueInstantiationsSrc(int iters) {
    return std::string("module C5302; "
                       "public class Program "
                       "  ") + kIdentity +
           "  public static method Main(): int "
           "    var t := 0; "
           "    var b := true; "
           "    var l := 3L; "
           "    for var i in 0..<" +
           std::to_string(iters) +
           " do "
           "      var a := Mk<int>(i); "
           "      var c := Mk<long>(l); "
           "      var d := Mk<bool>(b); "
           "      t := t + a; "
           "      if c = l then t := t + 1; end; "
           "      if d then t := t + 1; end; "
           "    end; "
           "    Console.WriteLine(t); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

// (C3) `Mk<string>` one frame deeper, called from a plain (non-generic)
// method whose parameter is itself the borrowed local. Keeps the fix from
// being specific to Main's shape. 690, same arithmetic as C1.
std::string nestedCallSrc(int iters) {
    return std::string("module C5303; "
                       "public class Program "
                       "  ") + kIdentity +
           "  static method Use(s: string): int "
           "    var x := Mk<string>(s); "
           "    return x.Length; "
           "  end Use; "
           "  public static method Main(): int "
           "    var t := 0; "
           "    for var i in 0..<" +
           std::to_string(iters) +
           " do "
           "      var p: string := \"w\" + i.ToString(); "
           "      t := t + Use(p); "
           "    end; "
           "    Console.WriteLine(t); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

// USER ROW 3 — #5300's second surface. TWO type parameters' worth of
// arguments where the SECOND is the one the callee returns
// (`Second<T>(a: T, b: T): T => b`) and BOTH are read after the call. §28.3's
// clause 4 makes BOTH plain parameters potential sources, so the returned one
// is a copy of `b` and `a` and `b` survive the call.
//
// This was labelled a control in the inherited WIP. It is NOT: MEASURED
// pre-fix on the lane base it reports
// `AddressSanitizer: 14543 byte(s) leaked in 798 allocation(s)` — the same
// root as user row 1 (the walker's blanket generic exclusion suppressing the
// borrowed locals' scope-end releases), and it passes only with the fix. It is
// a USER row, reclassified here; relabelling it a control would have made the
// aggregate "2 of 8 fail" claim wrong and hidden a real third surface.
// 2070 = 3 * 690.
std::string secondParamIsSourceSrc(int iters) {
    return "module C5304; "
           "public class Program "
           "  static method Second<T>(a: T, b: T): T => b; "
           "  public static method Main(): int "
           "    var t := 0; "
           "    for var i in 0..<" +
           std::to_string(iters) +
           " do "
           "      var p: string := \"w\" + i.ToString(); "
           "      var q: string := \"z\" + i.ToString(); "
           "      var x := Second<string>(p, q); "
           "      t := t + x.Length + p.Length + q.Length; "
           "    end; "
           "    Console.WriteLine(t); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

// (C5) A NON-generic `string`-returning method that concatenates a borrowed
// local with a fresh temporary — the shape the pre-existing callee-mint path
// already handled, which the new call-site gate must not double-count.
// 890 = sum of (2 + digits(i)): the "w" from the callee's `s` plus the "w"
// and digits of the argument.
std::string nonGenericConcatSrc(int iters) {
    return "module C5305; "
           "public class Program "
           "  static method Cat(s: string, t: string): string => s + t; "
           "  public static method Main(): int "
           "    var t0 := 0; "
           "    for var i in 0..<" +
           std::to_string(iters) +
           " do "
           "      var s := \"w\"; "
           "      var x := Cat(s, \"w\" + i.ToString()); "
           "      t0 := t0 + x.Length; "
           "    end; "
           "    Console.WriteLine(t0); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

// (C6) A non-generic method that MINTS its own string from an int — the
// +1-producer recogniser's existing positive case, unaffected by a gate
// keyed on a bare type parameter. 690.
std::string nonGenericStringReturnSrc(int iters) {
    return "module C5306; "
           "public class Program "
           "  static method Fresh(i: int): string => \"w\" + i.ToString(); "
           "  public static method Main(): int "
           "    var t0 := 0; "
           "    for var i in 0..<" +
           std::to_string(iters) +
           " do "
           "      var x := Fresh(i); "
           "      t0 := t0 + x.Length; "
           "    end; "
           "    Console.WriteLine(t0); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

constexpr int kIters = 200;
// sum over i in 0..<200 of (1 + decimal digits of i) = 200 + 490.
const char* kW690 = "690\n";

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// USER ROWS — each fails on the lane base and passes with the fix.
// ═══════════════════════════════════════════════════════════════════════════

// fix #5300 (§14.1 / §28.3 / §28.9, v0.313): a borrowed string local passed
// to a bare-type-parameter identity must keep its own reference, so the
// call's copy is an ADD and the local's scope-end release must still arm.
// Pre-fix: `7253 byte(s) leaked in 398 allocation(s)`.
BEZEL_TEST(generic_identity_bare_type_param_string_5300_5301,
           borrowed_arg_does_not_leak_spec_14_1_28_3_28_9) {
    BEZEL_EXPECT_TRUE(disposeExactlyOnce(borrowedArgSrc(kIters), kW690));
}

// fix #5301 (§14.1 / §28.3 / §28.9, v0.313): a fresh temporary passed to a
// bare-type-parameter identity is copied at the return, so the
// call-boundary release drops one reference and the result's own +1 keeps
// the value alive. Pre-fix: `heap-use-after-free ... in main`.
BEZEL_TEST(generic_identity_bare_type_param_string_5300_5301,
           fresh_arg_is_not_used_after_free_spec_14_1_28_3_28_9) {
    BEZEL_EXPECT_TRUE(disposeExactlyOnce(freshArgSrc(kIters), kW690));
}

// USER ROW 3 — #5300, second surface. Two plain type-parameter arguments,
// the SECOND returned, both read after the call. §28.3 clause 4 makes both
// potential sources, so neither dies in the callee. Pre-fix:
// `14543 byte(s) leaked in 798 allocation(s)`.
BEZEL_TEST(generic_identity_bare_type_param_string_5300_5301,
           two_params_returned_second_is_source_spec_14_1_28_3_28_9) {
    BEZEL_EXPECT_TRUE(disposeExactlyOnce(secondParamIsSourceSrc(kIters),
                                         "2070\n"));
}

// ═══════════════════════════════════════════════════════════════════════════
// CONTROLS — green before the fix and green after it.
// ═══════════════════════════════════════════════════════════════════════════

// (C1) The non-generic twin: the borrowed local and the runtime-built string
// with no generic call in the picture at all.
BEZEL_TEST(generic_identity_bare_type_param_string_5300_5301,
           control_no_generic_call_spec_14_1_28_3_28_9) {
    BEZEL_EXPECT_TRUE(disposeExactlyOnce(noGenericSrc(kIters), kW690));
}

// (C2) The same identity at T = int / long / bool. §28.9 makes `string` the
// runtime-managed type; a value substitution has no refcount, and the fix's
// `string`-primitive gate is what keeps this row out of the change.
BEZEL_TEST(generic_identity_bare_type_param_string_5300_5301,
           control_value_instantiations_int_long_bool_spec_14_1_28_3_28_9) {
    BEZEL_EXPECT_TRUE(disposeExactlyOnce(valueInstantiationsSrc(kIters),
                                         "20300\n"));
}

// (C3) `Mk<string>` one frame below Main, on a plain method's parameter.
BEZEL_TEST(generic_identity_bare_type_param_string_5300_5301,
           control_call_from_a_plain_method_spec_14_1_28_3_28_9) {
    BEZEL_EXPECT_TRUE(disposeExactlyOnce(nestedCallSrc(kIters), kW690));
}

// (C5) A non-generic `string` method concatenating a borrowed local with a
// fresh temporary — the pre-existing callee-mint path, which the new
// call-site gate must not double-count.
BEZEL_TEST(generic_identity_bare_type_param_string_5300_5301,
           control_non_generic_string_method_spec_14_1_28_3_28_9) {
    BEZEL_EXPECT_TRUE(disposeExactlyOnce(nonGenericConcatSrc(kIters), "890\n"));
}

// (C6) A non-generic method that mints its own string from an int.
BEZEL_TEST(generic_identity_bare_type_param_string_5300_5301,
           control_non_generic_string_return_spec_14_1_28_3_28_9) {
    BEZEL_EXPECT_TRUE(
        disposeExactlyOnce(nonGenericStringReturnSrc(kIters), kW690));
}
