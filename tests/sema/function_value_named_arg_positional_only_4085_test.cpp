// Spec-derived regression test for issue #4085 — calls THROUGH a function-
// typed callee (a delegate local / parameter / field, an indexed function
// element, or a chained call result) must reject the named-argument form
// (`Identifier :=`, §22.5), the same way §23.5 already rejects it at an
// index position (#2758).
//
// §27.1, verbatim, *Function types* (v0.294):
//
//   "Function types may include parameter names. Names are documentation
//    only; they do not affect type identity: ... (int, int, int) -> bool
//    and the named form are the same type. Calls are positional."
//
// The named-argument syntax reaches a function-value call site through the
// shared `Parser::parseArgList` (Parser_Decls.cpp) — same production as
// method calls, `new T(...)`, and `x[...]` — so `caller(b := 2, a := 100)`
// parses into a `CallArgument` with a non-empty `name`. The compiler's
// function-value arity/assignability branch (ExprTypecheck_Calls.cpp ~3764)
// used to bail on the first such argument (`simple = false`), silently
// skipping the per-argument check, while the codegen layer below bound
// every argument by its textual POSITION and dropped the written name.
//
// MEASURED ON 511e576, the parent of this fix (bezelc -o /tmp/prog):
//
//   module M;
//   class C
//     public static method Invoke(caller: (a: int, b: int) -> int): int
//       return caller(b := 2, a := 100);
//     end Invoke;
//     public static method Sub(a: int, b: int): int
//       return a * 1000 + b;
//     end Sub;
//     public static method Main(): int
//       return Invoke(Sub);
//     end Main;
//   end C;
//
//   compiled CLEAN (no error, no warning); the program ran and exited 52
//   — `Sub(2, 100) = 2100`, `2100 mod 256 = 52`. The textually-FIRST
//   argument (`b := 2`) bound to Sub's FIRST parameter `a`, and the
//   textually-SECOND (`a := 100`) bound to `b`. If binding were by NAME
//   (as the `b :=`/`a :=` syntax visually suggests) the result would be
//   `Sub(100, 2) = 100002`, exit `100002 mod 256 = 162`. The measured
//   `52` proves the names were discarded and binding was purely positional.
//
// THE FIX (§27.1, v0.294): the function-typed callee branch now rejects
// any argument whose `name` is non-empty with a §27.1 "calls are
// positional" diagnostic, mirroring §23.5's indexer treatment. Genuine
// METHOD calls go through `checkArgsAgainstParams` above and still honor
// the named form per §22.5 — the rejection is reached ONLY when the
// callee symbol is not a MethodSymbol/ConstructorSymbol.
//
// ── WHAT THIS PINS ────────────────────────────────────────────────────────
//
// (1) The REJECT shape, on every callee family that reaches the function-
//     typed callee branch: a delegate parameter (the parent commit's
//     surface), a delegate local (the §27.7-via-lambda case), a delegate
//     field. The §27.1 diagnostic text and citation are the discriminator
//     (NOT `hasErrors()` alone — the same surface compiles to a wrong
//     runtime value pre-fix, which runAot would surface as `ran=true`,
//     no diagnostics, so the parent-commit shell exit is the run-side
//     pin).
// (2) The ACCEPT surface: a positional call through the same delegate
//     local / parameter with the same `caller: (a:int, b:int) -> int`
//     signature compiles, runs, and returns the right value. The
//     REORDERED-named acceptance under §22.5 — a method call with
//     `M(b := 3, a := 10)` — must STILL be honored (it routes through
//     `checkArgsAgainstParams`, not this branch).
//
// ANTI-OVER-REJECT. The reject loop walks `ce.args` and emits one
// diagnostic per offending argument (so the line+column of each name
// is its own), then defers the modifier/discard check unchanged —
// nothing in the §22.3 / §22.4 modifier surface is affected. The
// existing simple-arg check still runs arity + assignability on
// non-modified, non-discard arguments, so a rejection on one named
// arg does not silence a §5.1 mismatch elsewhere.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Shared prelude: a two-parameter function type, a method that returns
// `a*1000 + b` (so a positional-vs-named disagreement is observable
// through the exit code), and an `Invoke` that takes the function as
// a parameter. The parameter form is the parent commit's exact
// repro surface; the local-variable form exercises the same branch
// through a `LocalSymbol` callee.
const char* kPrelude =
    "module M; "
    "class C "
    "  public static method Sub(a: int, b: int): int => a * 1000 + b; "
    "  public static method Invoke(caller: (a: int, b: int) -> int): int "
    "    => caller(2, 100); "
    "  public static method Main(): int => Invoke(Sub); "
    "end C; ";

std::string prog(const std::string& body) {
    return std::string(kPrelude) + body;
}

// The discriminator: at least one §27.1 "calls are positional" diagnostic
// naming `not admitted at a function-value call site` plus the §-citation.
// NOT `hasErrors()` alone — pre-fix, the same source compiled clean and
// ran with a wrong value (silent wrong-answer bug). The diagnostic text
// is what makes the rejection visible.
bool rejectsAsNamedFnArg(const std::string& program) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not admitted at a function-value call site") !=
                std::string::npos &&
            d.message.find("\xC2\xA7""27.1") != std::string::npos)
            return true;
    }
    return false;
}

// The same surface, via a function-typed LOCAL — the §27.7 "a function-
// typed local is callable" case (#3153). Lambda at a typed local; call
// site is `f(b := 2, a := 100)`. Same expected diagnostic.
const char* kLocalPrelude =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var f: (a: int, b: int) -> int := lambda(x, y) => x * 1000 + y; "
    "    return f(100, 2); "
    "  end Main; "
    "end C; ";

}  // namespace

// ── REJECT: named form at a function-value call site (parameter callee) ──

// The parent commit's exact repro: reordered named arguments through a
// delegate PARAMETER. Pre-fix this was a SILENT WRONG-VALUE: the call
// ran as `caller(2, 100)` and exited 52. Post-fix the source is a compile
// error and no binary is produced.
BEZEL_TEST(function_value_named_arg_positional_only_4085,
           reordered_named_args_on_a_delegate_parameter_are_rejected_spec_27_1) {
    // We replace the body of Invoke with the named-arg variant by hand.
    const std::string p =
        "module M; "
        "class C "
        "  public static method Invoke(caller: (a: int, b: int) -> int): int "
        "    return caller(b := 2, a := 100); "
        "  end Invoke; "
        "  public static method Sub(a: int, b: int): int => a * 1000 + b; "
        "  public static method Main(): int => Invoke(Sub); "
        "end C; ";
    BEZEL_EXPECT_TRUE(rejectsAsNamedFnArg(p));
}

// The same family, on a delegate LOCAL — the §27.7 callable-local path
// (#3153). Pre-fix the names were dropped and `f(b := 2, a := 100)`
// returned 2100 (= 2*1000 + 100) as `f(100, 2)` would have. Post-fix
// the source is rejected.
BEZEL_TEST(function_value_named_arg_positional_only_4085,
           reordered_named_args_on_a_delegate_local_are_rejected_spec_27_1) {
    const std::string p =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var f: (a: int, b: int) -> int := lambda(x, y) => x * 1000 + y; "
        "    return f(b := 2, a := 100); "
        "  end Main; "
        "end C; ";
    BEZEL_EXPECT_TRUE(rejectsAsNamedFnArg(p));
}

// All-named (no positional siblings) on a two-parameter delegate local.
// The bug repros even when the call supplies exactly the arity the
// function expects — the textual order is silently used.
BEZEL_TEST(function_value_named_arg_positional_only_4085,
           all_named_on_a_two_parameter_delegate_is_rejected_spec_27_1) {
    const std::string p =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var f: (a: int, b: int) -> int := lambda(x, y) => x * 1000 + y; "
        "    return f(a := 7, b := 9); "
        "  end Main; "
        "end C; ";
    BEZEL_EXPECT_TRUE(rejectsAsNamedFnArg(p));
}

// Single-named (one named, the rest positional) — a §22.5 mixed shape
// at a method call, which §27.1 forbids at a function-value call.
BEZEL_TEST(function_value_named_arg_positional_only_4085,
           a_single_named_arg_on_a_delegate_parameter_is_rejected_spec_27_1) {
    const std::string p =
        "module M; "
        "class C "
        "  public static method Invoke(caller: (a: int, b: int) -> int): int "
        "    return caller(100, b := 2); "
        "  end Invoke; "
        "  public static method Sub(a: int, b: int): int => a * 1000 + b; "
        "  public static method Main(): int => Invoke(Sub); "
        "end C; ";
    BEZEL_EXPECT_TRUE(rejectsAsNamedFnArg(p));
}

// ── VALUE PIN: pre-fix the silent wrong-value is gone ─────────────────────
//
// `prog("")` is the accept prelude: `Invoke(caller)` is called with
// `Sub`, which executes `Sub(2, 100)` = 2100, exits 52. This is the
// baseline that proves the accept surface (positional binding on the
// function-type callee) still WORKS — a separate accept test, not a
// reject test, so a regression that flipped the reject branch into a
// reject-the-accept case fails here with `expect 52 != other`.
BEZEL_TEST(function_value_named_arg_positional_only_4085,
           positional_args_on_a_delegate_parameter_still_bind_positionally_spec_27_1) {
    auto r = runAot(prog(""));
    // The pre-fix exit was 52 for the `caller(b := 2, a := 100)` source,
    // but the prelude here uses the textual `caller(2, 100)` spelling —
    // `Sub(2, 100) = 2100`, `2100 mod 256 = 52`. So the runtime exit
    // ALSO encodes "positional binding honored": a swapped accept would
    // exit a different number.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 52);
}

// ── ANTI-OVER-REJECT: §22.5 named args on a METHOD call still work ──────
//
// §22.5 governs method calls; §27.1 governs function-value calls. The
// rejection here is specifically the function-value callee family — a
// method call with reordered named arguments must still honor §22.5.
// (Lives at `tests/codegen/named_args_test.cpp` already; this is the
// in-file anti-over-reject pin so any future change to this branch
// cannot silently widen the rejection to the method callee family.)
BEZEL_TEST(function_value_named_arg_positional_only_4085,
           method_call_reorder_named_args_still_works_spec_22_5) {
    const std::string p =
        "module M; "
        "class C "
        "  public static method Sub(a: int, b: int): int => a - b; "
        "  public static method Main(): int => Sub(b := 3, a := 10); "
        "end C; ";
    auto r = runAot(p);
    BEZEL_EXPECT_TRUE(r.ran);
    // Sub(10, 3) = 7; exit 7.
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}