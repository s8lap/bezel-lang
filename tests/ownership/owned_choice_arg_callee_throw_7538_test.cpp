// A callee that THROWS after receiving an `owned` OWNING-CHOICE argument owns
// that argument: the caller must not dispose it again on the unwind edge
// (issue #7538; spec §28 / §28.3 *Exception-path discharge*, §15.3 *Parameter
// passing*, §35.4, v0.350).
//
// THE GOVERNING TEXT (docs/Bezel_Language_Spec.md, v0.350).
//
//   §28 — every heap object is disposed exactly once. §15.3 *Parameter
//   passing* — "for a move-only V, it is by value-move (transfer ownership).
//   The caller's argument expression is in moved-from state after the call."
//   §28.3 *Exception-path discharge* — the unwinding frame disposes what it
//   owns; "Exceptions therefore do not produce leaks."
//
// THE BUG (#7538). The caller zero-stores an owning-choice argument slot
// (`markMoveOnlyChoiceArgsMovedFrom`, LlvmBackend_Calls2.cpp) AFTER
// `lowerCallDispatch`, which places the store in the pending check's
// normal-return block. When the callee threw, its own unwind edge disposed its
// `owned` parameter, and the caller's still-populated slot was disposed again
// by the caller's scope pass. MEASURED at r30-c1 342da591 under
// `--sanitize=address` on row 1: `AddressSanitizer: heap-use-after-free`.
//
// THE FIX zero-stores the slot at the "call about to run" point of a
// user-method call (every argument evaluated, value already passed), so the
// store dominates the throw edge. Two controls pin what must NOT change: a
// LATER argument that throws (the call never runs; the caller still owns, row
// 7), and a callee `requires` that fails at entry (§22.16; the callee has not
// enrolled the parameter yet, so the caller still disposes it, row 8).
//
// THE ALLOW-LIST. The zero-store is sound only when every function the call
// can reach at run time enrolls `src` before anything in it can throw, so it
// is decided on the function the call EMITS and applied only to a call that is
// statically bound to a local block body (after the §11.8 witness redirect,
// rows 26-28). A call that DISPATCHES (interface or virtual member) keeps the
// post-dispatch sweep: its implementation may be an arrow body, and may live
// in a module that imports this one (rows 4, 24 and 31). Under checked
// contracts the entry invariant asked is the declaring class's own (rows
// 29-30).
//
// A STATIC callee's entry also runs its declaring type's lazy static
// initialization (§22.13, v0.350: "Once initialization begins for a type, all
// of its static fields are initialized before any of its methods can run on
// behalf of an outside observer"), ahead of the parameter's enrollment. A failing initializer raises
// TypeInitializationException there, so the caller must keep its disposal
// (rows 11-13; the plain call-site zero-store leaked 200 of 200 on each).
// The caller runs that initialization itself just ahead of the zero-store, so
// a callee whose initialization succeeds and whose BODY throws still disposes
// once: rows 14 (literal-only initializer, no call emitted) and 15 (a
// non-literal initializer that succeeds). Row 16 pins a `?.` call whose result
// is the owning choice the callee returns: the taker must dispose it once.
// Rows 17-18 pin §22.12 rule 1 for a GENERIC static callee called through its
// specialization: the caller-side initialization is not run for one, so its
// initializer runs exactly once. Rows 19-21 pin the ALIAS spelling
// (`type GS := G<string>; GS.Take(move s)`), which calls the erased template,
// whose entry runs the initialization: there the caller does run it first, and
// a failure leaves the caller owning its argument. Rows 22-25 pin an
// EXPRESSION-bodied callee that throws (static, instance, interface-dispatched,
// `?.`): it never enrolls its `owned` parameter for unwind disposal, so the
// caller keeps its disposal there.
//
// per §28.3, §15.3, §35.4, §22.16, §22.13, §22.12, §2.8.3, v0.350

#include <string>

#include "support/aot_support.hpp"  // runAotChecked — the strict AOT + ASan oracle
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::runAotChecked;
using bezel::test::runAotModulesChecked;

namespace {

const char* kDecls =
    "public class Res "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res._n := Res._n + 1; end destructor; "
    "end Res; "
    "public choice OChoice Text(owned r: Res), Empty; end OChoice; "
    "public interface ITk "
    "  method Take(owned src: OChoice); "
    "end ITk; "
    "public class Tk: ITk "
    "  public constructor() end constructor; "
    "  public method Take(owned src: OChoice) "
    "    if 1 < 2 then throw new InvalidOperationException(\"x\"); end if; "
    "  end Take; "
    "end Tk; ";

// `members` declares the callees; `body` is one `try` body; `exc` the caught
// exception type. 200 iterations; the destructor count is printed at the end.
std::string prog(const std::string& members, const std::string& body,
                 const std::string& exc = "InvalidOperationException") {
    return std::string("module M; ") + kDecls +
           " public class Program "
           " public static method Boom(): int "
           "   throw new InvalidOperationException(\"b\"); end Boom; "
           " public static method Other(owned src: OChoice) "
           "   using var x := src; end Other; " +
           members +
           " public static method Main() "
           "   var i := 0; "
           "   while i < 200 do "
           "     try " + body +
           "     catch var e: " + exc + " => i := i + 0; "
           "     end; "
           "     i := i + 1; "
           "   end while; "
           "   Console.WriteLine(Res._n); "
           " end Main; "
           "end Program;";
}

// `types` are declared ahead of `Program`; `members`, `body` and `exc` as for
// `prog`.
std::string progWithTypes(const std::string& types, const std::string& members,
                          const std::string& body,
                          const std::string& exc = "InvalidOperationException") {
    std::string p = prog(members, body, exc);
    const std::string head = "module M; ";
    return head + types + " " + p.substr(head.size());
}

const char* kThrowingTake =
    "public static method Take(owned src: OChoice) "
    "  if 1 < 2 then throw new InvalidOperationException(\"x\"); end if; "
    "end Take; ";

void expectRunOutput(const JitResult& r, const std::string& expected) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_28_3_issue_repro_throwing_callee_disposes_its_arg_once) {
    expectRunOutput(
        runAotChecked(prog(kThrowingTake,
            "var s := OChoice.Text(new Res()); Take(move s); ")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_28_3_throw_propagating_through_the_caller_disposes_once) {
    // The caller does not catch: the exception unwinds through `Mid`, whose
    // scope pass must find `s` already moved-from.
    expectRunOutput(
        runAotChecked(prog(
            std::string(kThrowingTake) +
            "public static method Mid() "
            "  var s := OChoice.Text(new Res()); Take(move s); "
            "end Mid; ",
            "Mid(); ")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_28_3_throwing_instance_method_disposes_its_arg_once) {
    expectRunOutput(
        runAotChecked(prog(
            "public constructor() end constructor; "
            "public method Take(owned src: OChoice) "
            "  if 1 < 2 then throw new InvalidOperationException(\"x\"); end if; "
            "end Take; ",
            "var p := new Program(); var s := OChoice.Text(new Res()); "
            "p.Take(move s); ")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_28_3_interface_dispatch_consuming_implementation_disposes_once) {
    // A DISPATCHED call (an interface member reached through the itable) keeps
    // the post-dispatch sweep alone: the implementation that runs is whichever
    // the receiver carries, including one in a module that imports this one,
    // so the caller cannot know whether it enrolls `src` before it can throw.
    // This row pins the sound half of that shape: the dispatched block body
    // consumes `src` and returns normally, and `src` is disposed exactly once
    // (a caller-side zero-store that was skipped AND a post-dispatch sweep
    // that was skipped would each show here, as a double dispose or a leak).
    // The throwing twin of this row stays a heap-use-after-free, as on
    // r30-c1, until the callee side enrolls at entry (#7707 and its prologue
    // sibling); row 24 pins the arrow-bodied implementation.
    expectRunOutput(
        runAotChecked(progWithTypes(
            "public class Tc: ITk "
            "  public constructor() end constructor; "
            "  public method Take(owned src: OChoice) "
            "    using var x := src; "
            "  end Take; "
            "end Tc; ",
            "",
            "var t: ITk := new Tc(); var s := OChoice.Text(new Res()); "
            "t.Take(move s); ")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_7_5_throwing_null_conditional_call_disposes_its_arg_once) {
    expectRunOutput(
        runAotChecked(prog(
            "public constructor() end constructor; "
            "public method Take(owned src: OChoice) "
            "  if 1 < 2 then throw new InvalidOperationException(\"x\"); end if; "
            "end Take; ",
            "var p: Program? := new Program(); "
            "var s := OChoice.Text(new Res()); p?.Take(move s); ")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_28_3_callee_moves_arg_onward_then_throws_disposes_once) {
    // `Other` takes and disposes the value; `Take` then throws with its own
    // parameter moved-from. The caller must not dispose it a second time.
    expectRunOutput(
        runAotChecked(prog(
            "public static method Take(owned src: OChoice) "
            "  Other(move src); "
            "  if 1 < 2 then throw new InvalidOperationException(\"x\"); end if; "
            "end Take; ",
            "var s := OChoice.Text(new Res()); Take(move s); ")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_28_3_later_argument_throw_leaves_the_caller_owning) {
    // Control: `Boom()` throws while the arguments are evaluated, so the call
    // never runs and the caller still owns `s`; it must dispose it.
    expectRunOutput(
        runAotChecked(prog(
            "public static method Take(owned src: OChoice, n: int) end Take; ",
            "var s := OChoice.Text(new Res()); Take(move s, Boom()); ")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_22_16_failed_requires_leaves_the_caller_disposing) {
    // Control: the §22.16 `requires` prologue throws before the callee's body
    // scope enrolls the parameter, so the caller's disposal is the only one.
    expectRunOutput(
        runAotChecked(prog(
            "public static method Take(owned src: OChoice, n: int) "
            "  requires(n > 5) "
            "end Take; ",
            "var s := OChoice.Text(new Res()); Take(move s, 1); ",
            "ContractViolationException")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_15_3_non_throwing_callee_control_disposes_once) {
    expectRunOutput(
        runAotChecked(prog(
            "public static method Take(owned src: OChoice) end Take; ",
            "var s := OChoice.Text(new Res()); Take(move s); ")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_14_type_parameter_formal_keeps_the_callers_disposal) {
    // Control for the call-site form: it runs while the callee's generic
    // substitution is installed, so `T` resolves to `OChoice` there, but the
    // callee does not enroll a formal declared as a bare type parameter. The
    // caller's disposal must stay the only one (a zero-store here leaked 200
    // of 200).
    expectRunOutput(
        runAotChecked(prog(
            "public static method Take<T>(owned src: T) end Take; ",
            "var s := OChoice.Text(new Res()); Take<OChoice>(move s); ")),
        std::string("200\n"));
}

// `kStatics` declares K, whose static initialization fails at first use:
// `X`'s initializer throws (§22.13). Every later use re-raises the saved
// TypeInitializationException (§22.7), so each of the 200 calls throws at the
// callee's entry, before its body scope enrolls `src`.
std::string staticInitProg(const std::string& kMembers) {
    std::string p = prog("",
                         "var s := OChoice.Text(new Res()); K.Take(move s); ",
                         "Exception");
    const std::string head = "module M; ";
    return head + "public class K " + kMembers + " end K; " +
           p.substr(head.size());
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_22_13_failed_static_init_at_static_callee_entry_keeps_callers_disposal) {
    // The reviewer's p4/statinit repro: `Take` reads the failing static.
    expectRunOutput(
        runAotChecked(staticInitProg(
            "public static method Boom(): int "
            "  throw new InvalidOperationException(\"x\"); end Boom; "
            "public static readonly X: int := K.Boom(); "
            "public static method Take(owned src: OChoice) "
            "  var y := K.X; end Take;")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_22_13_failed_static_init_untouched_by_body_keeps_callers_disposal) {
    // p4/statinit2: `Take`'s body never names a static; the entry's
    // initialization alone throws.
    expectRunOutput(
        runAotChecked(staticInitProg(
            "public static readonly X: int := K.Boom(); "
            "public static method Boom(): int "
            "  throw new InvalidOperationException(\"x\"); end Boom; "
            "public static method Take(owned src: OChoice) end Take;")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_22_13_failed_static_constructor_keeps_callers_disposal) {
    // The static-constructor spelling of the same entry throw.
    expectRunOutput(
        runAotChecked(staticInitProg(
            "static constructor() "
            "  if 1 < 2 then throw new InvalidOperationException(\"x\"); end if; "
            "end constructor; "
            "public static method Take(owned src: OChoice) end Take;")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_28_3_literal_static_init_callee_throw_disposes_its_arg_once) {
    // Precision control: K's only static initializer is a literal, so its
    // initialization cannot raise and the body's throw is the one that fires.
    // The callee disposes `src`; the caller must not.
    expectRunOutput(
        runAotChecked(staticInitProg(
            "public static _c: int := 0; "
            "public static method Take(owned src: OChoice) "
            "  K._c := K._c + 1; "
            "  if 1 < 2 then throw new InvalidOperationException(\"x\"); end if; "
            "end Take;")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_22_13_succeeding_static_init_then_body_throw_disposes_its_arg_once) {
    // K's initializer runs code but succeeds; `Take`'s body then throws after
    // enrolling `src`. The callee disposes it; the caller must not.
    expectRunOutput(
        runAotChecked(staticInitProg(
            "public static readonly X: int := K.One(); "
            "public static method One(): int return 1; end One; "
            "public static method Take(owned src: OChoice) "
            "  if 1 < 2 then throw new InvalidOperationException(\"x\"); end if; "
            "end Take;")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_7_5_null_conditional_call_returning_the_arg_disposes_once) {
    // §7.5: `p?.Pass(move src)` on a non-null receiver runs the call; `Pass`
    // returns its parameter, so `z` owns the value (§15.3).
    expectRunOutput(
        runAotChecked(prog(
            "public constructor() end constructor; "
            "public method Pass(owned src: OChoice): OChoice "
            "  return src; end Pass; ",
            "var p: Program? := new Program(); "
            "var s := OChoice.Text(new Res()); var z := p?.Pass(move s); ")),
        std::string("200\n"));
}

namespace {

// A generic G<T> whose per-instantiation static `_c` is initialized by a call
// that counts and prints its runs (`Cnt.Once`). `onceBody` is Once's body after
// the count; `take` is G's static `Take`; `main` is Main's body.
std::string genericInitProg(const std::string& onceBody,
                            const std::string& take, const std::string& main) {
    return std::string("module M; ") + kDecls +
           " public class Cnt "
           "  public static _runs: int := 0; "
           "  public static method Once(): int "
           "    Cnt._runs := Cnt._runs + 1; "
           "    Console.WriteLine($\"init run {Cnt._runs}\"); " + onceBody +
           "    return 5; "
           "  end Once; "
           "end Cnt; "
           "public class G<T> "
           "  public static _c: int := Cnt.Once(); " + take +
           "end G; "
           "public class Program "
           "  public static method Main() " + main +
           "  end Main; "
           "end Program;";
}

}  // namespace

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_22_12_generic_static_callee_runs_its_initializer_once) {
    // §22.12 rule 1, v0.350: "a type's static initializer runs exactly once
    // process-wide" (and §2.8.3 rule 4: one recorded initialization per
    // constructed generic type). `G<string>.Take(move s)` and a later read of
    // `G<string>._c` must share the one initialization: runs=1. A caller-side
    // `ensureinit` hoist ran the erased `G<T>` template's initializer as well
    // (MEASURED: `init run 1|body|init run 2|read c=5|runs=2`).
    //
    // Two orders are accepted, both with one run. §22.13 puts the
    // initialization before `Take` runs (the first); the current compiler runs
    // a generic instantiation's initializer at its first static read instead
    // (the second), a separate §22.13 ordering defect this row does not pin.
    JitResult r = runAotChecked(genericInitProg(
        "",
        "public static method Take(owned src: OChoice) "
        "  Console.WriteLine(\"body\"); end Take; ",
        "var s := OChoice.Text(new Res()); G<string>.Take(move s); "
        "Console.WriteLine($\"read c={G<string>._c}\"); "
        "Console.WriteLine($\"runs={Cnt._runs}\"); "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    const bool specOrder =
        r.stdoutText == "init run 1\nbody\nread c=5\nruns=1\n";
    const bool readOrder =
        r.stdoutText == "body\ninit run 1\nread c=5\nruns=1\n";
    BEZEL_EXPECT_TRUE(specOrder || readOrder);
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_22_12_initialized_generic_static_callee_is_not_reinitialized) {
    // The initializer throws if it ever runs a second time. `G<string>` is
    // initialized by the first read, so the later `Take` call must neither
    // re-run it nor raise: `Take` runs, disposes `src` once (Res._n = 1), and
    // nothing is caught. A second run (the hoist above) failed the call with
    // `caught` and `init run 2`.
    expectRunOutput(
        runAotChecked(genericInitProg(
            "if Cnt._runs > 1 then "
            "  throw new InvalidOperationException(\"second run\"); end if; ",
            "public static method Take(owned src: OChoice) "
            "  Console.WriteLine($\"body c={G<T>._c}\"); end Take; ",
            "Console.WriteLine($\"read c={G<string>._c}\"); "
            "try "
            "  var s := OChoice.Text(new Res()); G<string>.Take(move s); "
            "catch var e: Exception => Console.WriteLine($\"caught {e.Message}\"); "
            "end; "
            "Console.WriteLine(Res._n); ")),
        std::string("init run 1\nread c=5\nbody c=5\n1\n"));
}

namespace {

// The ALIAS spelling of a generic static callee: `type GS := G<string>;
// GS.Take(move s)`. Unlike `G<string>.Take`, which calls the specialized
// `G$Take$spec` (no entry `ensureinit`, #7684), this call reaches the erased
// template `bz$M.G$Take`, whose entry DOES run G's lazy static initialization
// (§22.13) ahead of enrolling `src`. `gMembers` declares G's initialization,
// which fails, and its `Take`; every one of the 200 calls throws at the
// callee's entry, so the caller must keep, and dispose, its argument.
std::string aliasInitProg(const std::string& gMembers) {
    std::string p = prog("",
                         "var s := OChoice.Text(new Res()); GS.Take(move s); ",
                         "Exception");
    const std::string head = "module M; ";
    return head + "type GS := G<string>; public class G<T> " + gMembers +
           " end G; " + p.substr(head.size());
}

}  // namespace

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_22_13_alias_generic_callee_failed_static_init_keeps_callers_disposal) {
    // The review's c4 probe. Gating the caller-side initialization on the
    // DECLARING type (generic) rather than the emitted call target (the
    // template) kept the zero-store and skipped the initialization: MEASURED
    // 3200 bytes leaked in 200 allocations, `Res._n` = 0.
    expectRunOutput(
        runAotChecked(aliasInitProg(
            "public static readonly _c: int := Program.Boom(); "
            "public static method Take(owned src: OChoice) end Take;")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_22_13_alias_generic_callee_failed_static_init_read_by_body_keeps_callers_disposal) {
    // The review's c6 probe: `Take`'s body also reads the failing static.
    expectRunOutput(
        runAotChecked(aliasInitProg(
            "public static readonly _c: int := Program.Boom(); "
            "public static method Take(owned src: OChoice) "
            "  var y := G<T>._c; end Take;")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_22_13_alias_generic_callee_failed_static_constructor_keeps_callers_disposal) {
    // The review's c7 probe: the static-constructor spelling.
    expectRunOutput(
        runAotChecked(aliasInitProg(
            "static constructor() "
            "  if 1 < 2 then throw new InvalidOperationException(\"x\"); end if; "
            "end constructor; "
            "public static method Take(owned src: OChoice) end Take;")),
        std::string("200\n"));
}

// An EXPRESSION-bodied callee (`=> Boom()`) does not enroll its `owned`
// parameter for unwind disposal (only a block body's root scope does), so when
// it throws, the caller's still-populated slot is the only disposal: the
// call-site zero-store must not apply. Each row was clean before #7538 and
// leaked 3200 bytes in 200 allocations with the call-site form applied.
BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_28_3_throwing_arrow_body_static_callee_keeps_callers_disposal) {
    expectRunOutput(
        runAotChecked(prog(
            "public static method Arrow(owned src: OChoice): int "
            "  => Program.Boom(); ",
            "var s := OChoice.Text(new Res()); var q := Arrow(move s); ")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_28_3_throwing_arrow_body_instance_callee_keeps_callers_disposal) {
    expectRunOutput(
        runAotChecked(progWithTypes(
            "public class Ak "
            "  public constructor() end constructor; "
            "  public method Arrow(owned src: OChoice): int => Program.Boom(); "
            "end Ak; ",
            "",
            "var k := new Ak(); var s := OChoice.Text(new Res()); "
            "var q := k.Arrow(move s); ")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_28_3_throwing_arrow_body_interface_implementation_keeps_callers_disposal) {
    // The interface method has no body; the dispatched implementation is an
    // arrow body.
    expectRunOutput(
        runAotChecked(progWithTypes(
            "public interface IAk "
            "  method Arrow(owned src: OChoice): int; "
            "end IAk; "
            "public class Ak: IAk "
            "  public constructor() end constructor; "
            "  public method Arrow(owned src: OChoice): int => Program.Boom(); "
            "end Ak; ",
            "",
            "var k: IAk := new Ak(); var s := OChoice.Text(new Res()); "
            "var q := k.Arrow(move s); ")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_7_5_throwing_arrow_body_null_conditional_call_keeps_callers_disposal) {
    expectRunOutput(
        runAotChecked(progWithTypes(
            "public class Ak "
            "  public constructor() end constructor; "
            "  public method Arrow(owned src: OChoice): int => Program.Boom(); "
            "end Ak; ",
            "",
            "var k: Ak? := new Ak(); var s := OChoice.Text(new Res()); "
            "var q := k?.Arrow(move s); ")),
        std::string("200\n"));
}

// The call-site zero-store is decided on the function the call EMITS, and only
// a statically bound call to a local block body qualifies. A call that
// DISPATCHES (an interface member or a virtual class member) reaches whichever
// implementation the receiver carries, including one in a module that imports
// this one, so it keeps the post-dispatch sweep alone: the caller still owns
// its value when the callee throws (the pre-#7538 behaviour for that shape).
// Row 4 above pins that a dispatched call still disposes exactly once when its
// implementation consumes the argument and returns normally.

// §11.8: `T.Take(move s)` through a constrained type parameter. Sema binds the
// callee to the interface's static member; lowerCallDispatch redirects the
// call to the concrete WITNESS, and the zero-store decision must read the
// witness, not the abstract member. `witness` is Kx's body for `Take`;
// `extra` is any other Kx member.
std::string staticIfaceProg(const std::string& witness,
                            const std::string& extra = "",
                            const std::string& exc = "InvalidOperationException") {
    return progWithTypes(
        "public class Tr "
        "  public static method Boom(): int "
        "    throw new InvalidOperationException(\"b\"); end Boom; "
        "end Tr; "
        "public interface ITake "
        "  static method Take(owned src: OChoice): int; "
        "end ITake; "
        "public class Kx: ITake " + extra +
        "  public static method Take(owned src: OChoice): int " + witness +
        "end Kx; "
        "public class Rn "
        "  public static method Run2<T>(): int where T: ITake "
        "    var s := OChoice.Text(new Res()); "
        "    return T.Take(move s); "
        "  end Run2; "
        "end Rn; ",
        "", "var q := Rn.Run2<Kx>(); ", exc);
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_11_8_static_interface_arrow_witness_throw_keeps_callers_disposal) {
    // r37-rev-l3d si1: the witness is an arrow body, so nothing in the callee
    // disposes `src` when it throws. Clean on r30-c1; 3200 bytes leaked in
    // 200 allocations when the decision read the interface member.
    expectRunOutput(runAotChecked(staticIfaceProg("=> Tr.Boom(); ")),
                    std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_11_8_static_interface_block_witness_throw_disposes_its_arg_once) {
    // r37-rev-l3d si2: a block-bodied witness enrolls `src` and disposes it on
    // its throw edge; the redirected call is statically bound, so the
    // zero-store applies. heap-use-after-free on r30-c1.
    expectRunOutput(
        runAotChecked(staticIfaceProg(
            "if 1 < 2 then throw new InvalidOperationException(\"x\"); end if; "
            "return 1; end Take; ")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_22_13_static_interface_witness_failed_static_init_keeps_callers_disposal) {
    // r37-rev-l3d si3: the WITNESS type's initializer fails, so the hoisted
    // `ensureinit` must be Kx's. Clean on r30-c1; 3200 bytes leaked in 200
    // allocations when the decision read the interface member (no hoist).
    expectRunOutput(
        runAotChecked(staticIfaceProg(
            "return 1; end Take; ",
            "public static readonly X: int := Tr.Boom(); ", "Exception")),
        std::string("200\n"));
}

// §22.16 under checked contracts (the oracle's mode): the entry invariant is
// the DECLARING class's. An `invariant` on an unrelated class does not make
// the callee's entry throw, so a statically bound block-bodied callee that
// throws still gets the zero-store (row 29). The callee's own class invariant,
// failing at entry, throws before `src` is enrolled, so the caller keeps its
// disposal (row 30).
BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_22_16_invariant_on_an_unrelated_class_does_not_keep_the_callers_disposal) {
    expectRunOutput(
        runAotChecked(progWithTypes(
            "public class Other "
            "  public _x: int := 1; "
            "  invariant(_x > 0); "
            "  public constructor() end constructor; "
            "end Other; "
            "public class Kv "
            "  public constructor() end constructor; "
            "  public method Take(owned src: OChoice): int "
            "    if 1 < 2 then throw new InvalidOperationException(\"x\"); end if; "
            "    return 1; "
            "  end Take; "
            "end Kv; ",
            "",
            "var o := new Other(); var k := new Kv(); "
            "var s := OChoice.Text(new Res()); var q := k.Take(move s); ")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_22_16_failed_entry_invariant_keeps_the_callers_disposal) {
    expectRunOutput(
        runAotChecked(progWithTypes(
            "public class Kv "
            "  public _x: int := 1; "
            "  invariant(_x > 0); "
            "  public constructor() end constructor; "
            "  public method Take(owned src: OChoice): int "
            "    using var t := src; return 1; "
            "  end Take; "
            "end Kv; ",
            "",
            "var k := new Kv(); k._x := 0; "
            "var s := OChoice.Text(new Res()); var q := k.Take(move s); ",
            "Exception")),
        std::string("200\n"));
}

// Cross-module (r37-rev-l3d `in/xmod`): module L calls `t.Take(move s)`
// through its interface; module M implements it with a throwing arrow body
// that L cannot see. The dispatched call keeps the post-dispatch sweep, so L
// still owns `s` when M's body throws. Clean on r30-c1; 3200 bytes leaked in
// 200 allocations when the decision scanned this module's methods only.
BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_28_3_cross_module_interface_arrow_implementation_throw_keeps_callers_disposal) {
    const char* kLib =
        "module Lib7538; "
        "public class Res "
        "  public static _n: int := 0; "
        "  public constructor() end constructor; "
        "  destructor Res._n := Res._n + 1; end destructor; "
        "end Res; "
        "public choice OChoice Text(owned r: Res), Empty; end OChoice; "
        "public interface IT "
        "  method Take(owned src: OChoice): int; "
        "end IT; "
        "public class Lib "
        "  public static method Go(t: IT): int "
        "    var s := OChoice.Text(new Res()); "
        "    return t.Take(move s); "
        "  end Go; "
        "end Lib;";
    const char* kApp =
        "module App7538; "
        "import Lib7538; "
        "public class K: IT "
        "  public constructor() end constructor; "
        "  public static method Boom(): int "
        "    throw new InvalidOperationException(\"b\"); end Boom; "
        "  public method Take(owned src: OChoice): int => K.Boom(); "
        "end K; "
        "public class Program "
        "  public static method Main() "
        "    var i := 0; "
        "    while i < 200 do "
        "      try var k := new K(); var q := Lib.Go(k); "
        "      catch var e: InvalidOperationException => i := i + 0; "
        "      end; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(Res._n); "
        "  end Main; "
        "end Program;";
    expectRunOutput(
        runAotModulesChecked({ModuleSource{"Lib7538", kLib},
                              ModuleSource{"App7538", kApp}},
                             "App7538"),
        std::string("200\n"));
}

// Dynamic invariant arms (r37-rev-l3e `in/rev_n` n01, n02, n04, n21, n24).
// §22.16 (v0.350): "an external call through a base-typed reference evaluates
// the full dynamic class invariant — base and derived conjunction". The entry
// check therefore runs a DERIVED class's invariant even when the declaring
// class has none, and it throws before `src` is enrolled, so the caller keeps
// its disposal. Clean on r30-c1; 3200 bytes in 200 allocations leaked when the
// decision consulted the declaring class's static invariant chain only.
BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_22_16_nonvirtual_base_method_on_derived_whose_invariant_fails) {
    expectRunOutput(runAotChecked(
    "module M;\n"
    "public class Res\n"
    "  public static _n: int := 0;\n"
    "  public constructor() end constructor;\n"
    "  destructor Res._n := Res._n + 1; end destructor;\n"
    "end Res;\n"
    "public choice OChoice Text(owned r: Res), Empty; end OChoice;\n"
    "public class Tr\n"
    "  public static method Boom(): int throw new InvalidOperationException(\"b\"); end Boom;\n"
    "end Tr;\n"
    "public class Bs\n"
    "  public constructor() end constructor;\n"
    "  public method Take(owned src: OChoice): int\n"
    "    return 1;\n"
    "  end Take;\n"
    "end Bs;\n"
    "public class Dv: Bs\n"
    "  public _ok: bool := true;\n"
    "  invariant(_ok);\n"
    "  public constructor() end constructor;\n"
    "  public method Break() _ok := false; end Break;\n"
    "end Dv;\n"
    "public class Program\n"
    "  public static method Main()\n"
    "    var i := 0;\n"
    "    while i < 200 do\n"
    "      try var d := new Dv(); d._ok := false; var s := OChoice.Text(new Res()); var q := d.Take(move s);\n"
    "      catch var e: Exception => i := i + 0;\n"
    "      end;\n"
    "      i := i + 1;\n"
    "    end while;\n"
    "    Console.WriteLine(Res._n);\n"
    "  end Main;\n"
    "end Program;\n"    ), std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_22_16_base_typed_reference_derived_invariant_fails) {
    expectRunOutput(runAotChecked(
    "module M;\n"
    "public class Res\n"
    "  public static _n: int := 0;\n"
    "  public constructor() end constructor;\n"
    "  destructor Res._n := Res._n + 1; end destructor;\n"
    "end Res;\n"
    "public choice OChoice Text(owned r: Res), Empty; end OChoice;\n"
    "public class Tr\n"
    "  public static method Boom(): int throw new InvalidOperationException(\"b\"); end Boom;\n"
    "end Tr;\n"
    "public class Bs\n"
    "  public constructor() end constructor;\n"
    "  public method Take(owned src: OChoice): int\n"
    "    return 1;\n"
    "  end Take;\n"
    "end Bs;\n"
    "public class Dv: Bs\n"
    "  public _ok: bool := true;\n"
    "  invariant(_ok);\n"
    "  public constructor() end constructor;\n"
    "end Dv;\n"
    "public class Program\n"
    "  public static method Main()\n"
    "    var i := 0;\n"
    "    while i < 200 do\n"
    "      try var d := new Dv(); d._ok := false; var b: Bs := d; var s := OChoice.Text(new Res()); var q := b.Take(move s);\n"
    "      catch var e: Exception => i := i + 0;\n"
    "      end;\n"
    "      i := i + 1;\n"
    "    end while;\n"
    "    Console.WriteLine(Res._n);\n"
    "  end Main;\n"
    "end Program;\n"    ), std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_22_16_derived_invariant_fails_before_a_throwing_base_body) {
    expectRunOutput(runAotChecked(
    "module M;\n"
    "public class Res\n"
    "  public static _n: int := 0;\n"
    "  public constructor() end constructor;\n"
    "  destructor Res._n := Res._n + 1; end destructor;\n"
    "end Res;\n"
    "public choice OChoice Text(owned r: Res), Empty; end OChoice;\n"
    "public class Tr\n"
    "  public static method Boom(): int throw new InvalidOperationException(\"b\"); end Boom;\n"
    "end Tr;\n"
    "public class Bs\n"
    "  public constructor() end constructor;\n"
    "  public method Take(owned src: OChoice): int\n"
    "    if 1 < 2 then throw new InvalidOperationException(\"x\"); end if;\n"
    "    return 1;\n"
    "  end Take;\n"
    "end Bs;\n"
    "public class Dv: Bs\n"
    "  public _ok: bool := true;\n"
    "  invariant(_ok);\n"
    "  public constructor() end constructor;\n"
    "end Dv;\n"
    "public class Program\n"
    "  public static method Main()\n"
    "    var i := 0;\n"
    "    while i < 200 do\n"
    "      try var d := new Dv(); d._ok := false; var s := OChoice.Text(new Res()); var q := d.Take(move s);\n"
    "      catch var e: Exception => i := i + 0;\n"
    "      end;\n"
    "      i := i + 1;\n"
    "    end while;\n"
    "    Console.WriteLine(Res._n);\n"
    "  end Main;\n"
    "end Program;\n"    ), std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_22_16_null_conditional_call_derived_invariant_fails) {
    expectRunOutput(runAotChecked(
    "module M;\n"
    "public class Res\n"
    "  public static _n: int := 0;\n"
    "  public constructor() end constructor;\n"
    "  destructor Res._n := Res._n + 1; end destructor;\n"
    "end Res;\n"
    "public choice OChoice Text(owned r: Res), Empty; end OChoice;\n"
    "public class Tr\n"
    "  public static method Boom(): int throw new InvalidOperationException(\"b\"); end Boom;\n"
    "end Tr;\n"
    "public class Bs\n"
    "  public constructor() end constructor;\n"
    "  public method Take(owned src: OChoice): int\n"
    "    return 1;\n"
    "  end Take;\n"
    "end Bs;\n"
    "public class Dv: Bs\n"
    "  public _ok: bool := true;\n"
    "  invariant(_ok);\n"
    "  public constructor() end constructor;\n"
    "end Dv;\n"
    "public class Program\n"
    "  public static method Main()\n"
    "    var i := 0;\n"
    "    while i < 200 do\n"
    "      try var d := new Dv(); d._ok := false; var b: Bs? := d; var s := OChoice.Text(new Res()); var q := b?.Take(move s);\n"
    "      catch var e: Exception => i := i + 0;\n"
    "      end;\n"
    "      i := i + 1;\n"
    "    end while;\n"
    "    Console.WriteLine(Res._n);\n"
    "  end Main;\n"
    "end Program;\n"    ), std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_arg_callee_throw_7538,
           spec_22_16_grand_derived_invariant_fails) {
    expectRunOutput(runAotChecked(
    "module M;\n"
    "public class Res\n"
    "  public static _n: int := 0;\n"
    "  public constructor() end constructor;\n"
    "  destructor Res._n := Res._n + 1; end destructor;\n"
    "end Res;\n"
    "public choice OChoice Text(owned r: Res), Empty; end OChoice;\n"
    "public class Tr\n"
    "  public static method Boom(): int throw new InvalidOperationException(\"b\"); end Boom;\n"
    "end Tr;\n"
    "public class Bs\n"
    "  public constructor() end constructor;\n"
    "  public method Take(owned src: OChoice): int\n"
    "    return 1;\n"
    "  end Take;\n"
    "end Bs;\n"
    "public class Mid: Bs\n"
    "  public constructor() end constructor;\n"
    "end Mid;\n"
    "public class Dv: Mid\n"
    "  public _ok: bool := true;\n"
    "  invariant(_ok);\n"
    "  public constructor() end constructor;\n"
    "end Dv;\n"
    "public class Program\n"
    "  public static method Main()\n"
    "    var i := 0;\n"
    "    while i < 200 do\n"
    "      try var d := new Dv(); d._ok := false; var m: Mid := d; var s := OChoice.Text(new Res()); var q := m.Take(move s);\n"
    "      catch var e: Exception => i := i + 0;\n"
    "      end;\n"
    "      i := i + 1;\n"
    "    end while;\n"
    "    Console.WriteLine(Res._n);\n"
    "  end Main;\n"
    "end Program;\n"    ), std::string("200\n"));
}

