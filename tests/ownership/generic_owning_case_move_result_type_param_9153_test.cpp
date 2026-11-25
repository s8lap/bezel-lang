// fix #9153 (§35.5 *The owning match*, §28.1 clause 3, §35.4 *The
// classification*, §19.3 *Patterns*, v0.359): a GENERIC function that holds an
// `owned` choice whose PAYLOAD TYPE IS A TYPE PARAMETER does not compile at
// all — the erased template's owning-choice disposal emits a vtable disposal
// for a payload whose storage is the `i32` placeholder, so the whole module
// fails LLVM verification.
//
// THE DEFECT. `Use<T>(owned b: Result<T, string>)` — the stdlib's
// `Result<T, E>` declares `Ok(owned value: T)` / `Error(owned error: E)`
// (stdlib §2.20), and §28.1 clause 3 fixes an `owned` bare-type-parameter
// marker's meaning PER INSTANTIATION. The ERASED TEMPLATE of the generic
// declaration has no instantiation: there the payload's storage is the `i32`
// placeholder (the lowered type of an unresolved type parameter), not a class
// reference — so there is nothing for the vtable path to dispatch on. Codegen
// emitted the disposal anyway (`emitOwningChoiceActiveCaseDisposal`), and the
// module failed to verify:
//
//   Both operands to ICmp instruction are not of the same type!
//     %8 = icmp eq i32 %ocd.owned.field, ptr null
//   Call parameter type does not match function signature!
//     %ocd.owned.field = extractvalue { i32 } %ocd.case.payload, 0
//    ptr  call void @bz_wait_for_borrows(i32 %ocd.owned.field)
//   bezelc: codegen error: IR verification failed (malformed IR emitted)
//
// MEASURED on the base (r30-c1 @ 5488b0e1c), the issue's own program: the
// compile fails with exactly that report. The defect needs no `case` at all —
// the `owned` PARAMETER's scope-end disposal is enough — and it fires for any
// owning choice with a type-parameter payload (the stdlib `Result`, a user
// choice), in either position of the type parameter.
//
// WHAT IS NOW TRUE. The generic declaration compiles, and the payload is
// disposed exactly once — at the CLASS instantiation, where §28.1 clause 3
// gives the `owned` marker "its ordinary transfer meaning", and not at a value
// or runtime-managed substitution, where the marker is "erased by the
// substitution" and the payload carries no obligation.
//
// TWO HALVES, ONE ROOT CAUSE. The compile failure was the disposal of the
// OWNING CHOICE VALUE (§35.4 "Disposal is defined by the active case"); the
// leak was the auto-drop of the owning match's TRANSFERRED PAYLOAD BINDING
// (§35.5: such a binding "is an owned local of the arm body's scope, and
// §28.3's standard rules apply to it on every arm-exit edge — falling off
// the arm still owning the value auto-drops it"). Both are decided PER
// INSTANTIATION, and the erased template is where the instantiation is not
// known — so both now read the instantiated payload type.
//
// OBSERVABILITY. Two oracles, both exact: the compile must succeed (the base
// does not compile at all), and the counting destructor's total must be 200 —
// one disposal per iteration — where the base's leak would print 0. 200
// iterations because a one-shot leak row can keep its block reachable from a
// live stack slot and read clean on BOTH sides.
//
// The rows are `runAotChecked` (the AOT + ASan/LSan oracle), so the leak is
// caught by the SANITIZER as well as by the count — the two error directions a
// discharge can introduce (a leak below the count, a double free above it)
// are separated at once.
//
// per §35.5, §28.1 clause 3, §35.4, §19.3, §28.3, v0.359

#include <string>

#include "support/aot_support.hpp"  // runAotChecked — the AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// A counting destructor, so the disposal is observable as a NUMBER rather
// than as a sanitizer report alone.
const char* kDecls =
    "public class Res "
    "  public static N: int := 0; "
    "  public static Made: int := 0; "
    "  public constructor() Res.Made := Res.Made + 1; end constructor; "
    "  destructor Res.N := Res.N + 1; end destructor; "
    "end Res; ";

// The generic declaration under test: an `owned` stdlib `Result<T, string>`
// parameter and an owning `case move` over it — the issue's own program.
const char* kGenericDecl =
    "public class G "
    "  public static method Use<T>(owned b: Result<T, string>): int "
    "    var k := 0; "
    "    case move b of Ok(v) => k := 1, Error(e) => k := 2 end case; "
    "    return k; "
    "  end Use; "
    "end G; ";

// The generic declaration with the TYPE PARAMETER in the ERROR position —
// `Result<string, T>` — the same shape, the other payload.
const char* kGenericDeclErr =
    "public class G "
    "  public static method Use<T>(owned b: Result<string, T>): int "
    "    var k := 0; "
    "    case move b of Ok(v) => k := 1, Error(e) => k := 2 end case; "
    "    return k; "
    "  end Use; "
    "end G; ";

// The same shape over a USER choice (the stdlib `Result` is not special: the
// erased payload layout is the generic choice's, not the stdlib's).
const char* kUserChoiceDecl =
    "public choice MyOpt<T> Some(owned v: T), None; "
    "end MyOpt; "
    "public class G "
    "  public static method Use<T>(owned b: MyOpt<T>): int "
    "    var k := 0; "
    "    case move b of Some(v) => k := 1, None => k := 2 end case; "
    "    return k; "
    "  end Use; "
    "end G; ";

// The NON-GENERIC twin — the control. §28.1 clause 3's class-or-interface
// assumption is what the generic body type-checks under; the twin is the
// shape that was always correct, and the fix must not move it.
const char* kNonGenericDecl =
    "public class G "
    "  public static method Use(owned b: Result<Res, string>): int "
    "    var k := 0; "
    "    case move b of Ok(v) => k := 1, Error(e) => k := 2 end case; "
    "    return k; "
    "  end Use; "
    "end G; ";

std::string prog(const std::string& decls, const std::string& body) {
    return std::string("module M; ") + kDecls + decls +
           "public class Program "
           "  public static method Main(): int " + body +
           "  end Main; "
           "end Program;";
}

// The issue's own call site: a class substitution (the payload is a class
// reference — the obligation is real), a value substitution and a
// runtime-managed one (the `owned` marker is erased — no obligation), and
// both arms.
const char* kClassBody =
    "var s := 0; "
    "var i := 0; "
    "while i < 200 do "
    "  s := s + G.Use<int>(Result<int, string>.Ok(5)); "
    "  s := s + G.Use<Res>(Result<Res, string>.Ok(new Res())); "
    "  s := s + G.Use<Res>(Result<Res, string>.Error(\"bad\")); "
    "  s := s + G.Use<string>(Result<string, string>.Ok(\"x\")); "
    "  i := i + 1; "
    "end while; "
    "Console.WriteLine($\"N={Res.N} Made={Res.Made} S={s}\"); "
    "return 0;";

// The VALUE substitutions only: no class reference anywhere, so no
// disposal obligation exists at these instantiations.
const char* kValueBody =
    "var s := 0; "
    "var i := 0; "
    "while i < 200 do "
    "  s := s + G.Use<int>(Result<int, string>.Ok(5)); "
    "  s := s + G.Use<string>(Result<string, string>.Ok(\"x\")); "
    "  i := i + 1; "
    "end while; "
    "Console.WriteLine($\"N={Res.N} Made={Res.Made} S={s}\"); "
    "return 0;";

}  // namespace

// ---------------------------------------------------------------------------
// THE ISSUE'S OWN PROGRAM — a GENERIC function owning a `Result<T, string>`.
// The base does not compile at all (the LLVM-verification failure), so this
// row is RED there; with the fix the program compiles, runs, and the class
// payload is disposed exactly once per iteration.
// ---------------------------------------------------------------------------
BEZEL_TEST(generic_owning_case_move_result_type_param_9153,
           spec_35_5_generic_owning_case_move_over_result_type_param_compiles_and_disposes) {
    auto r = runAotChecked(prog(kGenericDecl, kClassBody));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // One disposal per iteration (the class payload of the `Ok` arm), and
    // the sum of the four calls' results (1 + 1 + 2 + 1) over 200 iterations.
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("N=200 Made=200 S=1000\n"));
}

// ---------------------------------------------------------------------------
// THE SIBLING — the type parameter in the ERROR payload position.
// ---------------------------------------------------------------------------
BEZEL_TEST(generic_owning_case_move_result_type_param_9153,
           spec_35_5_generic_owning_case_move_over_result_string_type_param_disposes) {
    auto r = runAotChecked(prog(kGenericDeclErr,
                                "var s := 0; "
                                "var i := 0; "
                                "while i < 200 do "
                                "  s := s + G.Use<int>(Result<string, int>.Error(7)); "
                                "  s := s + G.Use<Res>(Result<string, Res>.Error(new Res())); "
                                "  i := i + 1; "
                                "end while; "
                                "Console.WriteLine($\"N={Res.N} Made={Res.Made} S={s}\"); "
                                "return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("N=200 Made=200 S=800\n"));
}

// ---------------------------------------------------------------------------
// THE SIBLING — a USER choice with a type-parameter payload (the stdlib
// `Result` is not special: the erased payload layout is the generic choice's).
// ---------------------------------------------------------------------------
BEZEL_TEST(generic_owning_case_move_result_type_param_9153,
           spec_35_5_generic_owning_case_move_over_a_user_choice_type_param_disposes) {
    auto r = runAotChecked(prog(kUserChoiceDecl,
                                "var s := 0; "
                                "var i := 0; "
                                "while i < 200 do "
                                "  s := s + G.Use<int>(MyOpt<int>.Some(5)); "
                                "  s := s + G.Use<Res>(MyOpt<Res>.Some(new Res())); "
                                "  s := s + G.Use<Res>(MyOpt<Res>.None); "
                                "  i := i + 1; "
                                "end while; "
                                "Console.WriteLine($\"N={Res.N} Made={Res.Made} S={s}\"); "
                                "return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("N=200 Made=200 S=800\n"));
}

// ---------------------------------------------------------------------------
// THE VALUE SUBSTITUTIONS — the erased template still has to VERIFY, and at
// a value / runtime-managed substitution §28.1 clause 3 erases the `owned`
// marker, so the payload carries no disposal obligation (the count stays 0).
// RED on the base for the same reason as the rows above: the base does not
// compile this program either.
// ---------------------------------------------------------------------------
BEZEL_TEST(generic_owning_case_move_result_type_param_9153,
           spec_28_1_clause_3_value_substitution_erases_the_owned_marker) {
    auto r = runAotChecked(prog(kGenericDecl, kValueBody));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // No class reference was ever created, so the counting destructor's
    // total is 0 — and the erased marker means it must stay 0 (a spurious
    // disposal here would read a value as a class reference).
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("N=0 Made=0 S=400\n"));
}

// ---------------------------------------------------------------------------
// THE BOUNDARY CONTROL — the NON-GENERIC twin, which was always correct.
// §28.1 clause 3's class-or-interface assumption is what the generic body
// type-checks under; the twin is the concrete shape, and the fix must leave
// it exactly as it was: the class payload disposed ONCE per iteration.
// (Green on the base by construction — the boundary control the lane
// contract allows: the check a corpus case cannot express, the 200-iteration
// destructor count, and RED under a plausible wrong fix — one that drops the
// arm-binding auto-drop, or disposes the payload twice.)
// ---------------------------------------------------------------------------
BEZEL_TEST(generic_owning_case_move_result_type_param_9153,
           control_35_5_non_generic_twin_still_disposes_the_payload_exactly_once) {
    auto r = runAotChecked(prog(kNonGenericDecl,
                                "var s := 0; "
                                "var i := 0; "
                                "while i < 200 do "
                                "  s := s + G.Use(Result<Res, string>.Ok(new Res())); "
                                "  s := s + G.Use(Result<Res, string>.Error(\"bad\")); "
                                "  i := i + 1; "
                                "end while; "
                                "Console.WriteLine($\"N={Res.N} Made={Res.Made} S={s}\"); "
                                "return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("N=200 Made=200 S=600\n"));
}
