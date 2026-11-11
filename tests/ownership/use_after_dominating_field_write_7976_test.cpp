// fix #7976 (spec §28.7 *Use after a dominating field write*, v0.357) — a
// use of a local borrow of an owned field's value after a disposing write of
// that field which dominates the use.
//
// ── THE ISSUE ──────────────────────────────────────────────────────────────
//
// #7976 — "overwriting an owned field while a borrow alias of it is live is a
// use-after-free". Its repro (`k24`):
//
//     var a := h.B!; h.B := new Box(); a.V := 3;
//
// MEASURED on the base (r30-c1 @ d60c087ca): the program COMPILED, and the
// store through `a` was a heap-use-after-free under AddressSanitizer in
// unchecked disposal mode (100 of 100 iterations reach it; the first one
// aborts) and a `DisposedAccessException` in checked disposal mode (§28.2).
//
// ── THE RULE ───────────────────────────────────────────────────────────────
//
// §28.7 (v0.357) *Use after a dominating field write*: "A local initialized
// from a direct read of an **owned** field — `var b := r.f;` or `const b :=
// r.f;`, `r` being `this` (written or implicit) or a local variable or
// non-`ref` parameter, `f` an owned instance field reachable there — borrows
// the field's current value. A **disposing write** of the same path — a
// reassignment `r.f := e;` (… the store of `null` at a nullable `f`
// included) or an explicit `r.f.Dispose()` … — that follows the borrow with
// **no invalidating operation between them** on any path from the borrow to
// the write … kills that value with certainty … A use of the local — a read,
// a member access, an argument, a re-borrow — that such a write
// **dominates** on every path … is a compile error … The write itself stays
// permitted (K5.6): what is rejected is the use the write makes certain to
// fault."
//
// The same paragraph names what STAYS runtime-checked: "A write on only some
// paths to the use, a window with an invalidating operation in it, a borrow
// held through another holder — an alias of the local, a closure … — a path
// of more than one field segment …". Those are the ADMITTING rows below: each
// is the raise row's shape with the one difference the sentence names, and
// each compiles on the base and with the fix.
//
// `h.B!` is read as the direct read `h.B`: §17.8 (v0.357) — "`x!` is purely a
// type-system assertion (with a runtime null check); it does not move, copy,
// or borrow beyond what reading `x` already does".
//
// Every row is on the strict AOT oracle (`runAot`): a raise row asserts the
// specific §28.7 diagnostic at a real source location, an admitting row
// asserts the program ran and printed what the spec says it prints.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::diagnosticMessageCount;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// The STABLE identifier of the rule's diagnostic: the paragraph's own title.
constexpr const char* kRule = "§28.7 *Use after a dominating field write*";

// `members` are spliced into `H`, `statics` into `C`, `body` into the loop of
// `Main`. The destructor has a body so `Box` is a disposal-carrying class.
std::string prog(const std::string& members, const std::string& statics,
                 const std::string& body) {
    return std::string(
               "module M;\n"
               "class Box\n"
               "  public V: int := 0;\n"
               "  public constructor() end constructor;\n"
               "  public method Get(): int => V;\n"
               "  destructor\n"
               "    Console.Write(\"\");\n"
               "  end destructor;\n"
               "end Box;\n"
               "class H\n"
               "  public owned B: Box;\n"
               "  public owned N: Box? := null;\n"
               "  public K: int := 0;\n"
               "  public constructor()\n"
               "    B := new Box(); N := new Box();\n"
               "  end constructor;\n"
               "  public method GetB(): Box => B;\n") +
           members +
           "end H;\n"
           "class Outer\n"
           "  public owned Inner: H;\n"
           "  public constructor() Inner := new H(); end constructor;\n"
           "end Outer;\n"
           "class C\n"
           "  public static method Nop() Console.Write(\"\"); end Nop;\n"
           "  public static method Make(): owned Box => new Box();\n" +
           statics +
           "  public static method Main(): int\n"
           "    var i := 0;\n"
           "    var total := 0;\n"
           "    while i < 3 do\n"
           "      using var h := new H();\n" +
           body +
           "      i := i + 1;\n"
           "    end;\n"
           "    Console.WriteLine($\"end {total}\");\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// A raise row: rejected at compile time, by THIS rule, exactly once, naming
// the local and the path.
void expectRejected(const std::string& src, const char* useOf,
                    const char* path) {
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kRule));
    BEZEL_EXPECT_EQ(diagnosticMessageCount(r, kRule), 1);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, useOf));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, path));
}

// An admitting row: compiles, runs to completion and prints `out`.
void expectRuns(const std::string& src, const char* out) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kRule));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(out));
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// THE ISSUE'S OWN REPROS
// ═══════════════════════════════════════════════════════════════════════════

// #7976's `k24`, verbatim. §28.7 (v0.357): the write `h.B := new Box()`
// follows the borrow `var a := h.B!` with nothing between them and dominates
// `a.V := 3`. MEASURED on the base: compiled; heap-use-after-free (ASan).
BEZEL_TEST(ownership_use_after_dominating_field_write_7976,
           issue_7976_k24_bang_borrow_overwrite_then_store_rejected_spec_28_7) {
    auto r = runAot(
        "module M;\n"
        "class Box\n"
        "  public V: int := 0;\n"
        "  public constructor() end constructor;\n"
        "  destructor\n"
        "    Console.WriteLine(\"fin\");\n"
        "  end destructor;\n"
        "end Box;\n"
        "class H\n"
        "  public owned B: Box? := null;\n"
        "  public constructor() end constructor;\n"
        "  public method Fill()\n"
        "    B := new Box();\n"
        "  end Fill;\n"
        "end H;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var i := 0;\n"
        "    while i < 100 do\n"
        "      using var h := new H(); h.Fill();\n"
        "      var a := h.B!; h.B := new Box(); a.V := 3; Console.Write(\"\");\n"
        "      i := i + 1;\n"
        "    end;\n"
        "    Console.WriteLine(\"end\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kRule));
    BEZEL_EXPECT_EQ(diagnosticMessageCount(r, kRule), 1);
    // "the diagnostic names the use, the borrow and the write" — all three
    // are on source line 21.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, ":21:40: "));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "use of `a` after the write to `h.B` at line 21"));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "`a` borrows the value `h.B` held at line 21"));
}

// #7976's `r24`, verbatim: the same shape over an `owned IEnumerator<int>?`
// field. §28.7 (v0.357) does not distinguish the field's type — "`f` an owned
// instance field".
BEZEL_TEST(ownership_use_after_dominating_field_write_7976,
           issue_7976_r24_enumerator_field_overwrite_then_movenext_rejected_spec_28_7) {
    expectRejected(
        "module M;\n"
        "class G\n"
        "  public static method Gen(): IEnumerable<int>\n"
        "    try\n"
        "      yield return 1;\n"
        "      yield return 2;\n"
        "    finally\n"
        "      Console.WriteLine(\"fin\");\n"
        "    end;\n"
        "  end Gen;\n"
        "  public static method It(): IEnumerator<int>\n"
        "    try\n"
        "      yield return 5;\n"
        "      yield return 6;\n"
        "    finally\n"
        "      Console.WriteLine(\"fin\");\n"
        "    end;\n"
        "  end It;\n"
        "end G;\n"
        "class H\n"
        "  public owned En: IEnumerator<int>? := null;\n"
        "  public method Fill()\n"
        "    En := G.Gen().GetEnumerator();\n"
        "  end Fill;\n"
        "\n"
        "end H;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var i := 0;\n"
        "    while i < 100 do\n"
        "      using var h := new H(); h.Fill(); var ok := h.En!.MoveNext();\n"
        "      var a := h.En!; h.En := G.It(); var m := a.MoveNext();\n"
        "      i := i + 1;\n"
        "    end;\n"
        "    Console.WriteLine(\"end\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "use of `a` after the write to `h.En` at line 32",
        "`a` borrows the value `h.En` held at line 32");
}

// ═══════════════════════════════════════════════════════════════════════════
// RAISE ROWS — the borrow's spelling, the write's form, the use's form
// ═══════════════════════════════════════════════════════════════════════════

// "`var b := r.f;`" over a NON-nullable field, the use a field READ.
BEZEL_TEST(ownership_use_after_dominating_field_write_7976,
           nonnull_field_borrow_overwrite_then_read_rejected_spec_28_7) {
    expectRejected(prog("", "",
                        "      var a := h.B; h.B := new Box(); total := total + a.V;\n"),
                   "use of `a` after the write to `h.B`", "held at line 31");
}

// "or `const b := r.f;`".
BEZEL_TEST(ownership_use_after_dominating_field_write_7976,
           const_borrow_overwrite_then_read_rejected_spec_28_7) {
    expectRejected(prog("", "",
                        "      const a := h.B; h.B := new Box(); total := total + a.V;\n"),
                   "use of `a` after the write to `h.B`", "held at line 31");
}

// "the store of `null` at a nullable `f` included".
BEZEL_TEST(ownership_use_after_dominating_field_write_7976,
           store_of_null_then_use_rejected_spec_28_7) {
    expectRejected(prog("", "",
                        "      var a := h.N!; h.N := null; a.V := 3;\n"),
                   "use of `a` after the write to `h.N`", "held at line 31");
}

// "A use of the local — a read, a member access, an argument, a re-borrow":
// the re-borrow `var c := a` is itself the rejected use.
BEZEL_TEST(ownership_use_after_dominating_field_write_7976,
           reborrow_after_write_rejected_spec_28_7) {
    expectRejected(prog("", "",
                        "      var a := h.B; h.B := new Box(); var c := a; total := total + c.V;\n"),
                   "use of `a` after the write to `h.B`", "held at line 31");
}

// "`r` being `this` (written or implicit)": the borrow reads the bare field,
// the write names it through `this`.
BEZEL_TEST(ownership_use_after_dominating_field_write_7976,
           implicit_this_borrow_explicit_this_write_rejected_spec_28_7) {
    expectRejected(prog("  public method Swap()\n"
                        "    var a := B;\n"
                        "    this.B := new Box();\n"
                        "    a.V := 3;\n"
                        "  end Swap;\n",
                        "", "      h.Swap();\n"),
                   "use of `a` after the write to `B` at line 20",
                   "`a` borrows the value `B` held at line 19");
}

// "or a local variable or non-`ref` parameter": a PARAMETER root.
BEZEL_TEST(ownership_use_after_dominating_field_write_7976,
           parameter_root_overwrite_then_use_rejected_spec_28_7) {
    expectRejected(prog("",
                        "  public static method P(p: H)\n"
                        "    var a := p.B;\n"
                        "    p.B := new Box();\n"
                        "    a.V := 3;\n"
                        "  end P;\n",
                        "      C.P(h);\n"),
                   "use of `a` after the write to `p.B` at line 28",
                   "`a` borrows the value `p.B` held at line 27");
}

// "or an explicit `r.f.Dispose()` … where §28.6 admits it" — inside the
// declaring type.
BEZEL_TEST(ownership_use_after_dominating_field_write_7976,
           explicit_field_dispose_then_use_rejected_spec_28_7) {
    expectRejected(prog("  public method Swap()\n"
                        "    var a := B;\n"
                        "    B.Dispose();\n"
                        "    a.V := 3;\n"
                        "  end Swap;\n",
                        "", "      h.Swap();\n"),
                   "use of `a` after the explicit disposal of `B` at line 20",
                   "held at line 19");
}

// The window holds NO invalidating operation: a primitive computation, a
// store of a primitive into a plain field and an `if` over them are none of
// §28.7's forms ("no call …, accessor read or write, user-defined operator,
// function-value invocation or `await` …, no `yield return`, no disposal of
// another value, no other write to `r.f`, no `move` of any field path, and no
// write to `r`").
BEZEL_TEST(ownership_use_after_dominating_field_write_7976,
           inert_window_between_borrow_and_write_rejected_spec_28_7) {
    expectRejected(prog("", "",
                        "      var a := h.B;\n"
                        "      var k := 1 + 2;\n"
                        "      h.K := k;\n"
                        "      if i > 1 then total := total + 1; end if;\n"
                        "      h.B := new Box();\n"
                        "      a.V := 3;\n"),
                   "use of `a` after the write to `h.B` at line 35",
                   "held at line 31");
}

// "The window ends where the write's right-hand side begins": a CALL as the
// stored operand does not reopen the window.
BEZEL_TEST(ownership_use_after_dominating_field_write_7976,
           write_whose_operand_is_a_call_rejected_spec_28_7) {
    expectRejected(prog("", "",
                        "      var a := h.B; h.B := C.Make(); a.V := 3;\n"),
                   "use of `a` after the write to `h.B`", "held at line 31");
}

// "that such a write **dominates** on every path": the use sits in a branch
// and in a loop the write precedes.
BEZEL_TEST(ownership_use_after_dominating_field_write_7976,
           use_inside_branch_and_loop_after_write_rejected_spec_28_7) {
    expectRejected(prog("", "",
                        "      var a := h.B;\n"
                        "      h.B := new Box();\n"
                        "      var j := 0;\n"
                        "      while j < 2 do\n"
                        "        if j > 0 then total := total + a.V; end if;\n"
                        "        j := j + 1;\n"
                        "      end;\n"),
                   "use of `a` after the write to `h.B` at line 32",
                   "held at line 31");
}

// The rule is stated over a borrow, a write and a use, not over a kind of
// body: a block-bodied property getter holds the same three.
BEZEL_TEST(ownership_use_after_dominating_field_write_7976,
           property_getter_body_overwrite_then_use_rejected_spec_28_7) {
    expectRejected(prog("  public property P: int\n"
                        "    get => do var a := B; B := new Box(); return a.V; end;\n"
                        "  end;\n",
                        "", "      total := total + h.P;\n"),
                   "use of `a` after the write to `B` at line 19",
                   "`a` borrows the value `B` held at line 19");
}

// … and so does a block-bodied lambda, whose parameter is the path's root.
BEZEL_TEST(ownership_use_after_dominating_field_write_7976,
           lambda_body_overwrite_then_use_rejected_spec_28_7) {
    expectRejected(prog("", "",
                        "      var f := lambda(p: H): int => do var a := p.B; p.B := new Box(); return a.V; end;\n"
                        "      total := total + f(h);\n"),
                   "use of `a` after the write to `p.B` at line 31",
                   "`a` borrows the value `p.B` held at line 31");
}

// ═══════════════════════════════════════════════════════════════════════════
// ADMITTING ROWS — "Use the borrow before the write", and the residual list
// ═══════════════════════════════════════════════════════════════════════════

// "Use the borrow before the write" — the remedy the paragraph names.
BEZEL_TEST(ownership_use_after_dominating_field_write_7976,
           use_before_write_control_runs_spec_28_7) {
    expectRuns(prog("", "",
                    "      var a := h.B; a.V := 3; total := total + a.V; h.B := new Box();\n"),
               "end 9\n");
}

// "A write on only some paths to the use" stays runtime-checked. The write's
// branch is never taken here, so the program is also fault-free.
BEZEL_TEST(ownership_use_after_dominating_field_write_7976,
           write_on_only_some_paths_control_runs_spec_28_7) {
    expectRuns(prog("", "",
                    "      var a := h.B;\n"
                    "      if i > 200 then h.B := new Box(); end if;\n"
                    "      a.V := 3; total := total + a.V;\n"),
               "end 9\n");
}

// "the local … not assigned between the borrow and the use": `a` is rebound
// to the NEW value after the write.
BEZEL_TEST(ownership_use_after_dominating_field_write_7976,
           local_reassigned_after_write_control_runs_spec_28_7) {
    expectRuns(prog("", "",
                    "      var a := h.B; h.B := new Box(); a := h.B; a.V := 3; total := total + a.V;\n"),
               "end 9\n");
}

// "A disposing write of the SAME path": a write of another field, and of the
// same field of another object, kill nothing `a` borrows.
BEZEL_TEST(ownership_use_after_dominating_field_write_7976,
           write_of_another_path_control_runs_spec_28_7) {
    expectRuns(prog("", "",
                    "      using var h2 := new H();\n"
                    "      var a := h.B;\n"
                    "      h.N := new Box();\n"
                    "      h2.B := new Box();\n"
                    "      a.V := 3; total := total + a.V;\n"),
               "end 9\n");
}

// "Use the borrow before the write" inside a property getter and a lambda.
BEZEL_TEST(ownership_use_after_dominating_field_write_7976,
           accessor_and_lambda_use_before_write_control_runs_spec_28_7) {
    expectRuns(prog("  public property P: int\n"
                    "    get => do var a := B; a.V := 3; var v := a.V; B := new Box(); return v; end;\n"
                    "  end;\n",
                    "",
                    "      var f := lambda(p: H): int => do var a := p.B; a.V := 3; var v := a.V; p.B := new Box(); return v; end;\n"
                    "      total := total + h.P + f(h);\n"),
               "end 18\n");
}

// A loop whose body uses the borrow and THEN writes the field: on the first
// trip the use precedes the write, so the write does not dominate it.
BEZEL_TEST(ownership_use_after_dominating_field_write_7976,
           loop_use_then_write_control_runs_spec_28_7) {
    expectRuns(prog("", "",
                    "      var a := h.B;\n"
                    "      var j := 0;\n"
                    "      while j < 1 do\n"
                    "        a.V := 3; total := total + a.V;\n"
                    "        h.B := new Box();\n"
                    "        j := j + 1;\n"
                    "      end;\n"),
               "end 9\n");
}

// `??=` stores only into a slot holding `null`: it displaces no value, so it
// is not a disposing write.
BEZEL_TEST(ownership_use_after_dominating_field_write_7976,
           coalescing_store_control_runs_spec_28_7) {
    expectRuns(prog("", "",
                    "      var a := h.N!; h.N ??= new Box(); a.V := 3; total := total + a.V;\n"),
               "end 9\n");
}

// The residual list — "a window with an invalidating operation in it, a
// borrow held through another holder — an alias of the local …, a path of
// more than one field segment … stay under the runtime-checked posture".
// Each member below holds one residual shape and must COMPILE; none is
// called, because executing one is the fault §28.2 backstops at run time.
BEZEL_TEST(ownership_use_after_dominating_field_write_7976,
           residual_shapes_stay_admitted_spec_28_7) {
    expectRuns(prog("",
                    // a call in the window
                    "  public static method R1(h: H)\n"
                    "    var a := h.B; C.Nop(); h.B := new Box(); a.V := 3;\n"
                    "  end R1;\n"
                    // a borrow held through an alias of the local
                    "  public static method R2(h: H)\n"
                    "    var a := h.B; var c := a; h.B := new Box(); c.V := 3;\n"
                    "  end R2;\n"
                    // a path of more than one field segment
                    "  public static method R3(o: Outer)\n"
                    "    var a := o.Inner.B; o.Inner.B := new Box(); a.V := 3;\n"
                    "  end R3;\n"
                    // a borrow a call returned is not a direct read
                    "  public static method R4(h: H)\n"
                    "    var a := h.GetB(); h.B := new Box(); a.V := 3;\n"
                    "  end R4;\n"
                    // two different writes on two branches: neither dominates
                    "  public static method R5(h: H, c: bool)\n"
                    "    var a := h.B;\n"
                    "    if c then h.B := new Box(); else h.B := new Box(); end if;\n"
                    "    a.V := 3;\n"
                    "  end R5;\n",
                    "      total := total + 1;\n"),
               "end 3\n");
}
