// Regression tests for issue #9155 — a BARE method-group identifier in VALUE
// position carried NO type, so every consumer of that value degraded.
//
// SPEC — settled, not underspecified.
//
//   §2.7 *Name resolution for dotted names* (v0.359), rule 1 and its table
//   (v0.359): "Resolve the leftmost segment `A` as a bare name: local scope,
//   then the enclosing type's accessible members **in the position given
//   below**, …" — and the table's "Bare value read" row: "**between local
//   scope and module declarations** | member outranks same-named
//   top-level/imported type". So inside the declaring type, the BARE name of
//   one of its members denotes that member.
//
//   §27.6 *Method references* (v0.359): "Method references work for static
//   methods, instance methods (the instance is captured at formation …),
//   properties, and extension methods." and "A reference's **formed type** is
//   the callee's own signature read as a function type — the receiver slot
//   removed for a bound reference …".
//
//   §27.7 *Calling function values* (v0.359): a call through a function-typed
//   value invokes the value.
//
//   So the bare spelling `P` — a static method of the enclosing class — is a
//   METHOD REFERENCE whose formed type is the callee's own signature read as a
//   function type, exactly as the qualified spelling `Program.P` is. The
//   QUALIFIED spelling published that formed type (resolveMemberAccess's
//   §27.6 branch, issue #436); the BARE spelling published NOTHING.
//
// MEASURED PRE-FIX (r30-c1 @ 658bf14cc, the base). The missing type is not
// cosmetic — it DELETED CODE, silently, at exit 0:
//
//   | program                                   | base                              |
//   |-------------------------------------------|-----------------------------------|
//   | `var f := P; t := t + f(1);` in a loop   | warning #2961, prints `0`         |
//   | `Console.WriteLine(f(1));` in a loop      | warning #3079, prints NOTHING    |
//   | `var f := P;` at the METHOD body's top    | warning #3079, prints NOTHING    |
//   | `Console.WriteLine(P);`                  | prints NOTHING (no diagnostic)   |
//   | `var o: Object := P;`                    | SEGFAULT (exit 139)              |
//
//   The QUALIFIED twins of the first three rows (`Program.P`) printed `33`,
//   `11` and `12` — the bare spelling is the ONLY difference, and the scope
//   (a block body vs the method body's top level) is NOT: the bare spelling
//   is broken in BOTH. (The issue's own control row used the qualified
//   spelling, which is why the scope looked like the trigger.)
//
//   The mechanism: a local's inferred type IS its initializer's exprType
//   (sema's LocalDecl arm: `lsym->declType = ctx_.exprType(*ld->initializer)`),
//   so an untyped initializer leaves the local untyped; codegen's
//   call-through-a-local arm then has no function type to call and lowers the
//   call to NOTHING — the enclosing statement emits nothing (#3079) or the
//   assignment's store is dropped (#2961).
//
// THE FIX (sema, ExprTypecheck_Exprs.cpp's bare-identifier arm): publish the
// bare method group's formed type — `buildMethodRefType`'s, the SAME builder
// the §5.1 `checkAssignable` arm already uses for this spelling (fix #4897) —
// so the bare spelling and the qualified spelling now carry the same type.
//
// The rows below are RED on the base and GREEN with the fix. The twins the
// issue lists as already correct (`Other.P`, a receiver-qualified instance
// method `s.P`, a lambda literal) are NOT gtest rows: they are green on the
// base, so they are corpus candidates (see the lane's
// `probes/corpus-candidates/` directory).

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"         // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::checkedToolchainSkipped;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Issue #9155's own repro, VERBATIM: a STATIC method group of the ENCLOSING
// class, bound to a local inside a BLOCK scope (the loop body) and called
// through that local.
const char* kIssueRepro =
    "module M; "
    "public class Program "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 0 ..< 3 do "
    "      var f := P; "
    "      t := t + f(1); "
    "    end for; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "  static method P(i: int): int => i + 10; "
    "end Program;";

// The SAME binding, called as the ARGUMENT of a statement: the argument fails
// to lower, so the whole statement emits nothing — not even the line
// terminator (#3079). The base printed NOTHING at all.
const char* kConsoleArgumentForm =
    "module M; "
    "public class Program "
    "  public static method Main(): int "
    "    for var i in 0 ..< 3 do "
    "      var f := P; "
    "      Console.WriteLine(f(1)); "
    "    end for; "
    "    return 0; "
    "  end Main; "
    "  static method P(i: int): int => i + 10; "
    "end Program;";

// The INSTANCE method group of the enclosing class: §27.6 captures the
// instance at formation, so the formed type is the callee's signature with the
// receiver slot REMOVED (`() -> int`) — the same shape the qualified
// `this.Inst` forms. The base printed `0`.
const char* kInstanceGroup =
    "module M; "
    "public class Program "
    "  public static method Main(): int "
    "    var p := new Program(); "
    "    Console.WriteLine(p.Run()); "
    "    return 0; "
    "  end Main; "
    "  public method Run(): int "
    "    var t := 0; "
    "    for var i in 0 ..< 3 do "
    "      var f := Inst; "
    "      t := t + f(1); "
    "    end for; "
    "    return t; "
    "  end Run; "
    "  public method Inst(i: int): int => i + 10; "
    "end Program;";

// 200 iterations: the closure CELL the bare method group forms is heap-allocated
// per iteration (a 64-byte cell), so the loop amplifies a leak of it 200×.
// §28.9: a function value's lifetime is managed by the runtime — the cell must
// be released exactly once per formation.
const char* kTwoHundredIterations =
    "module M; "
    "public class Program "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 0 ..< 200 do "
    "      var f := P; "
    "      t := t + f(1); "
    "    end for; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "  static method P(i: int): int => i + 10; "
    "end Program;";

}  // namespace

// ===========================================================================
// The issue's own repro: the bare static method group, bound in a BLOCK scope,
// called through the local.  RED on the base (prints `0`, warning #2961).
// ===========================================================================
BEZEL_TEST(bare_method_group_local_call_9155,
           bare_static_method_group_local_call_in_block_scope_27_6) {
    JitResult r = runAot(kIssueRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // 3 x P(1) = 3 x 11 = 33.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("33\n"));
}

// ===========================================================================
// The same binding as a STATEMENT ARGUMENT: the base dropped the whole
// statement (warning #3079) and printed NOTHING — not even a newline.
// ===========================================================================
BEZEL_TEST(bare_method_group_local_call_9155,
           bare_static_method_group_local_call_as_statement_argument_27_6) {
    JitResult r = runAot(kConsoleArgumentForm);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("11\n11\n11\n"));
}

// ===========================================================================
// The INSTANCE method group of the enclosing class — the same root cause, the
// other callee kind (§27.6: the instance is captured at formation).
// ===========================================================================
BEZEL_TEST(bare_method_group_local_call_9155,
           bare_instance_method_group_local_call_in_block_scope_27_6) {
    JitResult r = runAot(kInstanceGroup);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("33\n"));
}

// ===========================================================================
// The leak leg: 200 formations of the bare method group, each a heap cell the
// runtime owns.  Under the checked (ASan/LSan) oracle the cell must be
// released exactly once per formation and the answer must be right.
// ===========================================================================
BEZEL_TEST(bare_method_group_local_call_9155,
           bare_static_method_group_local_call_200_iterations_no_leak_27_6) {
    JitResult r = runAotChecked(kTwoHundredIterations);
    // A toolchain skip would make the row pass for the wrong reason.
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // 200 x P(1) = 200 x 11 = 2200.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2200\n"));
}
