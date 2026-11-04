// Regression tests for #8031 -- a `case` EXPRESSION used as a local-decl
// initializer never captured its owned scrutinee temporary, so the temporary
// LEAKED (224 bytes in 3 allocations) on the normal edge AND on the exception
// edge.
//
// THE DEFECT (measured). `CaseExpr::disposeOwnedScrutinee` is the ownership
// pass's K8 full-expression-boundary mark for the `case`-expression scrutinee.
// Codegen's `lowerCaseExpr` reads it: when it is set, the scrutinee is
// captured into an entry alloca (`scrutOwnedSlot`), a dispose-stack entry is
// registered for it, and the construct's normal-edge reclaim
// (`freeUnboundOwnedOwningCollectionTemp`) releases it at the construct's
// completion.
//
// BUT THE MARK WAS ONLY SET WHEN THE OWNERSHIP PASS REACHED THE CASE
// EXPRESSION THROUGH `handleDischarge` -- and a local-decl initializer is not
// walked that way. So in the `var r := (case ...)` form the mark was not set,
// the capture never happened, and nothing ever released the temporary. The
// STATEMENT-POSITION TWIN was fine (`case Mk() of ... end case;`): the
// `CaseStmt` mark comes from the CFG block walk
// (`src/ownership/Ownership.cpp`), which does not depend on the expression
// context -- so the statement form captured and reclaimed, and only the
// expression form leaked.
//
// THE SPEC (language spec v0.353, measured -- the numbering did NOT move).
//
//   §28.3.1 "The ownership kernel (K1-K9)", K8 "The full-expression
//   boundary", clause K8.3 (spec line 6987):
//     "A `case` scrutinee's boundary is the whole construct -- pattern
//      bindings are borrows of the scrutinee, and the temporary survives arm
//      selection, guard evaluation, and the selected arm's completion -- IN
//      STATEMENT POSITION EXACTLY AS IN EXPRESSION POSITION."
//
//   §28.3.11 "Unbound owned temporaries" (spec line 7705), the clause
//   "**`case` scrutinees extend the boundary.**":
//     "For a `case` whose scrutinee produces an unbound owned temporary, the
//      boundary is K8.3's: the whole `case` construct, IN STATEMENT POSITION
//      EXACTLY AS IN EXPRESSITION POSITION, which is what keeps §19.3's
//      pattern bindings -- borrows whose source is the scrutinee -- alive
//      through the arm body."
//
// So the boundary is the CONSTRUCT, not the statement: the expression
// position is required to behave exactly as the statement position does, and
// the scrutinee's temporary is the construct's to release. The compiler was
// implementing the statement half only.
//
// THE FIX. The mark is a property of the NODE and of the scrutinee's producer
// classification (an owned producer: a `new`, an owned-returning call, a
// `move`, ... -- `producerState` reads the AST shape and the static type, and
// consults no ownership dataflow), so it does not depend on the POSITION the
// construct is reached from. It is now set from every expression position:
// the discharge walk, the read walk (the path a local-decl initializer, an
// assignment RHS, a `return` operand and a call argument take), and the
// ownership pass's one syntactic walk that reaches every `case` expression in
// every METHOD-BODY expression position.
//
// THE DECLARATION-SITE POSITIONS. A `case` expression that lives in a
// DECLARATION-SITE expression -- a field initializer, a static field
// initializer, an auto-property initializer, a parameter's default value, a
// property's expression body, a property accessor's body, a `requires` /
// `ensures` clause, a class `invariant` -- is not part of any method body,
// so none of the three method-body walkers reached it, and the construct's
// owned scrutinee temporary LEAKED there too (MEASURED: 224 bytes in 3
// allocations per evaluation; 22400 bytes in 300 allocations for a position
// evaluated 100 times). The ownership pass now marks the K8 boundary for
// those positions as well, through the SAME predicate.
//
// THE NESTED EXPRESSION POSITIONS. A `case` expression can also be NESTED
// inside another expression -- inside a range's endpoint (`1..(case Mk() of
// _ => 2)`) or inside a record `with(...)` override value (`new R() with
// (v := (case Mk() of _ => 1))`). Those nestings were NOT reached either:
// the read walk (`checkRead`) has a `RangeExpr` arm and a `WithExpr` arm, but
// the ownership pass's ONE syntactic walk over a method body -- the §35.5
// arm-borrow-source collector -- did NOT, so a `case` nested in either was
// invisible to it, and the declaration-site reach (which drives that very
// collector) inherited the hole. The collector now carries the same two arms
// `checkRead` has, so the nesting is reached at every position.
//
// THE ORACLE. Every row measures a COUNT -- the number of allocations
// LeakSanitizer reports as leaked, read out of LSan's own "SUMMARY:
// AddressSanitizer: <bytes> byte(s) leaked in <allocations> allocation(s)."
// line via the harness's own parser
// (`bezel::test::aot_detail::parseLeakSummary`). The expected count is ZERO;
// on the pre-fix compiler the SINGLE-SHOT leak rows read 3 allocations / 224
// bytes (the `bz_array_t` header, its backing buffer and the owned element)
// and the rows evaluated 100 times read 300 allocations / 22400 bytes. The
// rows also run under `runAotChecked`, whose LSan leg turns a leak into a
// failed row, so the count and the verdict agree.
//
// Every leak row loops (or is amplified) -- a one-shot leak repro is a
// vacuous oracle.
//
// The CONTROL rows are the shapes that must NOT change: the statement-
// position twin (already captured -- a double free here would be caught), and
// a NAMED scrutinee (a local, not an unbound temporary: the construct does
// not own it, the local's own scope-end disposal does, and the local must
// still be usable after the construct).
//
// NOT A DECLARATION-SITE POSITION: a class-level `const` initializer cannot
// hold a `case` expression at all -- sema rejects the shape ("const `K` must
// be initialized with a compile-time-constant expression", §22.12), so there
// is no leak to fix there and no row for it.
//
// Spec: §28.3.1 K8 / K8.3, §28.3.11 (v0.353); §19.2 "Arms" (the expression-
// position `case`); §9.5 (`with`); §16.1 "Construction" (range expressions);
// §22.12 (`const` initializers); stdlib §4.5 (`OwningArray<T>` -- the owning
// family).

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Every diagnostic the harness produced, concatenated -- the sanitizer report
// the AOT harness embeds in the diagnostic message when LSan fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// The program under test. `Mk()` returns an OWNED `OwningArray<Cell>` -- an
// unbound owned temporary in every position that consumes it as a `case`
// scrutinee -- so the construct is the temporary's holder (K8.3).
std::string prog(const std::string& body) {
    return
        "module M; import System; import System.Collections.Generic; "
        "class Cell "
        "  public v: int := 0; "
        "end Cell; "
        "class C "
        "  public static method Mk(): owned OwningArray<Cell> "
        "    var a := new OwningArray<Cell>(); "
        "    a.Add(new Cell()); "
        "    return a; "
        "  end Mk; "
        "  public static method Main(): int "
        + body +
        "  end Main; "
        "end C;";
}

// The program under test for the DECLARATION-SITE positions: the same `Mk()`
// owned-materializer, the given member declarations spliced into class `H`,
// and the given `Main` body.
std::string declProg(const std::string& decls, const std::string& body) {
    return
        "module M; import System; import System.Collections.Generic; "
        "class Cell "
        "  public v: int := 0; "
        "end Cell; "
        "class H "
        "  public static method Mk(): owned OwningArray<Cell> "
        "    var a := new OwningArray<Cell>(); "
        "    a.Add(new Cell()); "
        "    return a; "
        "  end Mk; "
        + decls +
        "  public static method Main(): int "
        + body +
        "  end Main; "
        "end H;";
}

// The RECORD variant -- a record's own component initializer is a
// declaration-site position too, and the record's `Main` lives in a
// companion class.
std::string recordProg(const std::string& decls, const std::string& body) {
    return
        "module M; import System; import System.Collections.Generic; "
        "class Cell "
        "  public v: int := 0; "
        "end Cell; "
        "record R "
        "  public static method Mk(): owned OwningArray<Cell> "
        "    var a := new OwningArray<Cell>(); "
        "    a.Add(new Cell()); "
        "    return a; "
        "  end Mk; "
        + decls +
        "end R; "
        "class C "
        "  public static method Main(): int "
        + body +
        "  end Main; "
        "end C;";
}

// The PRIMARY-CONSTRUCTOR variant -- the parameter list belongs to the TYPE's
// header, so it cannot be spliced in as a member declaration.
std::string primaryCtorProg(const std::string& headerParams,
                             const std::string& decls, const std::string& body) {
    return
        "module M; import System; import System.Collections.Generic; "
        "class Cell "
        "  public v: int := 0; "
        "end Cell; "
        "class H(" + headerParams + ") "
        "  public static method Mk(): owned OwningArray<Cell> "
        "    var a := new OwningArray<Cell>(); "
        "    a.Add(new Cell()); "
        "    return a; "
        "  end Mk; "
        + decls +
        "  public static method Main(): int "
        + body +
        "  end Main; "
        "end H;";
}

// The NESTED-EXPRESSION variant -- the same `Mk()` owned-materializer, a
// RECORD `R2` to be the `with(...)` source, the given member declarations
// spliced into class `H`, and the given `Main` body. A `case` nested in a
// range's endpoint or in a record `with(...)` override value needs the
// record in scope.
std::string nestedProg(const std::string& decls, const std::string& body) {
    return
        "module M; import System; import System.Collections.Generic; "
        "class Cell "
        "  public v: int := 0; "
        "end Cell; "
        "record R2 "
        "  public v: int; "
        "end R2; "
        "class H "
        "  public static method Mk(): owned OwningArray<Cell> "
        "    var a := new OwningArray<Cell>(); "
        "    a.Add(new Cell()); "
        "    return a; "
        "  end Mk; "
        + decls +
        "  public static method Main(): int "
        + body +
        "  end Main; "
        "end H;";
}

// The METHOD-BODY nested variant -- the record `R2` and the `Mk()`
// owned-materializer, with the given `Main` body in class `C`.
std::string nestedBodyProg(const std::string& body) {
    return
        "module M; import System; import System.Collections.Generic; "
        "class Cell "
        "  public v: int := 0; "
        "end Cell; "
        "record R2 "
        "  public v: int; "
        "end R2; "
        "class C "
        "  public static method Mk(): owned OwningArray<Cell> "
        "    var a := new OwningArray<Cell>(); "
        "    a.Add(new Cell()); "
        "    return a; "
        "  end Mk; "
        "  public static method Main(): int "
        + body +
        "  end Main; "
        "end C;";
}

// THE ORACLE. `leakedAllocations` is the COUNT of allocations LeakSanitizer
// reports as leaked: 0 when there is no SUMMARY line (LSan prints one only
// when it finds leaks), and the summary's own number when there is one. The
// pre-fix compiler read 3 here (224 bytes) for the single-shot rows and 300
// (22400 bytes) for the 100-iteration row.
//
// The row also asserts that the program ran to completion with the expected
// output and that NO sanitizer report of any kind is present, so a double
// free (which ASan reports as "attempting double-free") fails the row too.
void expectNoLeak(const JitResult& r, const char* out, int exitCode = 0) {
    // The row must NOT have skipped: a skipped row asserts nothing.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary = bezel::test::aot_detail::parseLeakSummary(
        diagnosticText(r), leakedBytes, leakedAllocs);
    const unsigned long long leakedAllocations = sawSummary ? leakedAllocs : 0ull;
    const unsigned long long leakedByteCount = sawSummary ? leakedBytes : 0ull;
    // The COUNT, printed so a failing row's log carries the NUMBER the leak
    // oracle measured (the pre-fix compiler reads 3 allocations / 224 bytes
    // here, from `bz_array_alloc`).
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocations
                  << " allocation(s) / " << leakedByteCount
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(leakedAllocations, 0ull);
    BEZEL_EXPECT_EQ(leakedByteCount, 0ull);
    // No sanitizer report of any kind: a double free would land here.
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") ==
                      std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
}

}  // namespace

// =========================================================================
// The LEAK rows -- the expression positions that did not capture.
// =========================================================================

// THE ISSUE'S OWN REPRO, verbatim: a `case` EXPRESSION as a LOCAL-DECL
// INITIALIZER. The scrutinee `Mk()` is an unbound owned temporary; the
// construct is its holder (K8.3), so the construct must release it at its
// completion. Pre-fix: 3 allocations / 224 bytes leaked.
BEZEL_TEST(case_expr_owned_scrutinee_capture_8031,
           local_decl_initializer_captures_owned_scrutinee_spec_28_3_11) {
    JitResult r = runAotChecked(prog(
        "    var r := (case Mk() of _ => 1); "
        "    Console.WriteLine(r); "
        "    return 0; "));
    expectNoLeak(r, "1\n");
}

// The CALL-ARGUMENT position -- the same construct reached through the
// discharge walk's argument handling rather than through a binding. Pre-fix:
// 3 allocations / 224 bytes leaked.
BEZEL_TEST(case_expr_owned_scrutinee_capture_8031,
           call_argument_captures_owned_scrutinee_spec_28_3_11) {
    JitResult r = runAotChecked(prog(
        "    Console.WriteLine((case Mk() of _ => 1)); "
        "    return 0; "));
    expectNoLeak(r, "1\n");
}

// The ASSIGNMENT-RHS position. Pre-fix: 3 allocations / 224 bytes leaked.
BEZEL_TEST(case_expr_owned_scrutinee_capture_8031,
           assignment_rhs_captures_owned_scrutinee_spec_28_3_11) {
    JitResult r = runAotChecked(prog(
        "    var r := 0; "
        "    r := (case Mk() of _ => 1); "
        "    Console.WriteLine(r); "
        "    return 0; "));
    expectNoLeak(r, "1\n");
}

// The RETURN-OPERAND position -- the construct's value is returned, so the
// scrutinee's temporary must be released BEFORE the return.
//
// MEASURED: this position was ALREADY clean on the pre-fix compiler (the
// return path reaches the construct through the discharge walk, not the read
// walk), so this row is a CONTROL row, not a leak row: it guards the
// construct's value against a double release and against a leak that the
// return's own ownership transfer could introduce.
BEZEL_TEST(case_expr_owned_scrutinee_capture_8031,
           return_operand_captures_owned_scrutinee_spec_28_3_11) {
    JitResult r = runAotChecked(prog(
        "    return (case Mk() of _ => 1); "));
    expectNoLeak(r, "", 1);
}

// THE AMPLIFIED ROW -- 100 iterations of the issue's own shape. A one-shot
// leak repro is a vacuous oracle; here the pre-fix compiler reads 300
// allocations / 22400 bytes (100 iterations x 3 allocations).
BEZEL_TEST(case_expr_owned_scrutinee_capture_8031,
           loop_amplifies_the_capture_100_iterations_spec_28_3_11) {
    JitResult r = runAotChecked(prog(
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 100 do "
        "      total := total + (case Mk() of _ => 1); "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(total); "
        "    return 0; "));
    expectNoLeak(r, "100\n");
}

// =========================================================================
// The CONTROL rows -- the shapes that must NOT change (and the one
// expression position that was already clean).
// =========================================================================

// The STATEMENT-POSITION TWIN -- `case Mk() of ... end case;`. This shape was
// ALWAYS clean (its mark comes from the CFG block walk), and the fix must
// leave it clean: a second release here would be a double free, which this
// row would catch.
BEZEL_TEST(case_expr_owned_scrutinee_capture_8031,
           statement_position_twin_stays_single_release_spec_28_3_11) {
    JitResult r = runAotChecked(prog(
        "    case Mk() of "
        "      _ => Console.WriteLine(\"arm\"); "
        "    end case; "
        "    return 0; "));
    expectNoLeak(r, "arm\n");
}

// A NAMED scrutinee -- the scrutinee is a LOCAL, not an unbound temporary,
// so the construct does NOT own it: the local's own scope-end disposal frees
// it, exactly once. The row asserts both that nothing leaks and that the
// local is STILL USABLE after the construct (a premature release would show
// up as a use-after-free / a DisposedAccessException).
BEZEL_TEST(case_expr_owned_scrutinee_capture_8031,
           named_scrutinee_is_not_reclaimed_by_the_construct_spec_28_3_11) {
    JitResult r = runAotChecked(prog(
        "    using var arr := Mk(); "
        "    var r := (case arr of _ => 1); "
        "    Console.WriteLine(r); "
        "    Console.WriteLine(arr.Count); "
        "    return 0; "));
    expectNoLeak(r, "1\n1\n");
}

// A named scrutinee that is an ordinary (non-`using`) local -- disposed by
// the §28.3 scope-end auto-drop, once. The construct must not release it.
BEZEL_TEST(case_expr_owned_scrutinee_capture_8031,
           named_local_scrutinee_is_not_reclaimed_by_the_construct_spec_28_3_11) {
    JitResult r = runAotChecked(prog(
        "    var arr := Mk(); "
        "    var r := (case arr of _ => 1); "
        "    Console.WriteLine(r); "
        "    return 0; "));
    expectNoLeak(r, "1\n");
}

// =========================================================================
// The DECLARATION-SITE positions -- the ones the method-body walkers never
// reached.
//
// A `case` EXPRESSION written in a declaration-site expression is not part of
// any method body, so the discharge walk, the read walk and the §35.5
// collector -- the three walkers that reach a `case` expression inside a
// method body -- never saw it, and the construct's owned scrutinee temporary
// LEAKED at each of these positions (MEASURED at the pre-fix tree: 224 bytes
// in 3 allocations per evaluation). Every row below asserts the POSITIVE
// observable -- `leakedAllocations == 0` and `leakedByteCount == 0` -- plus
// the program's exact stdout and exit code.
//
// Each row AMPLIFIES: 100 evaluations of the position, so the pre-fix
// compiler reads 300 allocations / 22400 bytes and the row fails on a NUMBER,
// not on a yes/no. (The one-shot static-field row is the exception: a static
// initializer runs once per program, so it cannot be amplified that way.)
// =========================================================================

// An INSTANCE FIELD INITIALIZER -- `public r: int := (case Mk() of _ => 1);`.
// The initializer runs once per constructed instance, so 100 constructions
// amplify the leak 100-fold.
BEZEL_TEST(case_expr_owned_scrutinee_capture_8031,
           field_initializer_captures_owned_scrutinee_spec_28_3_11) {
    JitResult r = runAotChecked(declProg(
        "  public r: int := (case Mk() of _ => 1); ",
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 100 do "
        "      var h := new H(); "
        "      total := total + h.r; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(total); "
        "    return 0; "));
    expectNoLeak(r, "100\n");
}

// A STATIC FIELD INITIALIZER -- `public static readonly r: int := (case …);`.
// The static initializer runs ONCE per program (§22.13's static-init model),
// so this row is the one single-shot leak row: the pre-fix compiler reads 3
// allocations / 224 bytes here.
BEZEL_TEST(case_expr_owned_scrutinee_capture_8031,
           static_field_initializer_captures_owned_scrutinee_spec_28_3_11) {
    JitResult r = runAotChecked(declProg(
        "  public static readonly r: int := (case Mk() of _ => 1); ",
        "    Console.WriteLine(H.r); "
        "    return 0; "));
    expectNoLeak(r, "1\n");
}

// An AUTO-PROPERTY INITIALIZER -- `public property R: int := (case …);`. The
// initializer runs once per constructed instance (the auto-property's
// compiler-generated backing field is written by it), so 100 constructions
// amplify the leak 100-fold.
BEZEL_TEST(case_expr_owned_scrutinee_capture_8031,
           auto_property_initializer_captures_owned_scrutinee_spec_28_3_11) {
    JitResult r = runAotChecked(declProg(
        "  public property R: int := (case Mk() of _ => 1); ",
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 100 do "
        "      var h := new H(); "
        "      total := total + h.R; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(total); "
        "    return 0; "));
    expectNoLeak(r, "100\n");
}

// A RECORD COMPONENT INITIALIZER -- the record's own component initializer is
// a declaration-site position too (§9.2's body-record component), and it runs
// once per constructed record, so 100 constructions amplify the leak 100-fold.
BEZEL_TEST(case_expr_owned_scrutinee_capture_8031,
           record_component_initializer_captures_owned_scrutinee_spec_28_3_11) {
    JitResult r = runAotChecked(recordProg(
        "  public r: int := (case Mk() of _ => 1); ",
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 100 do "
        "      var rr := new R(); "
        "      total := total + rr.r; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(total); "
        "    return 0; "));
    expectNoLeak(r, "100\n");
}

// A METHOD PARAMETER'S DEFAULT VALUE -- `F(x: int := (case Mk() of _ => 1))`.
// The default is evaluated at the CALL SITE (the omitted-argument path), so
// 100 calls that omit the argument amplify the leak 100-fold.
BEZEL_TEST(case_expr_owned_scrutinee_capture_8031,
           method_parameter_default_captures_owned_scrutinee_spec_28_3_11) {
    JitResult r = runAotChecked(declProg(
        "  public static method F(x: int := (case Mk() of _ => 1)): int "
        "    return x; "
        "  end F; ",
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 100 do "
        "      total := total + F(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(total); "
        "    return 0; "));
    expectNoLeak(r, "100\n");
}

// A CONSTRUCTOR PARAMETER'S DEFAULT VALUE -- the omitted-argument path again,
// at the constructor's own call site, so 100 constructions amplify the leak
// 100-fold.
BEZEL_TEST(case_expr_owned_scrutinee_capture_8031,
           constructor_parameter_default_captures_owned_scrutinee_spec_28_3_11) {
    JitResult r = runAotChecked(declProg(
        "  public r: int; "
        "  public constructor(x: int := (case Mk() of _ => 1)) "
        "    r := x; "
        "  end constructor; ",
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 100 do "
        "      var h := new H(); "
        "      total := total + h.r; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(total); "
        "    return 0; "));
    expectNoLeak(r, "100\n");
}

// A PRIMARY-CONSTRUCTOR PARAMETER'S DEFAULT VALUE -- the parameter list
// belongs to the TYPE's header (§22.10), not to any method body, so it is a
// declaration-site position in its own right. 100 constructions amplify the
// leak 100-fold.
BEZEL_TEST(case_expr_owned_scrutinee_capture_8031,
           primary_constructor_parameter_default_captures_owned_scrutinee_spec_28_3_11) {
    JitResult r = runAotChecked(primaryCtorProg(
        "r: int := (case Mk() of _ => 1)",
        "",
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 100 do "
        "      var h := new H(); "
        "      total := total + h.r; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(total); "
        "    return 0; "));
    expectNoLeak(r, "100\n");
}

// A PROPERTY'S EXPRESSION BODY -- `property R: int => (case Mk() of _ => 1);`
// (§23.1). The body is evaluated on every read, so 100 reads amplify the
// leak 100-fold.
BEZEL_TEST(case_expr_owned_scrutinee_capture_8031,
           property_expression_body_captures_owned_scrutinee_spec_28_3_11) {
    JitResult r = runAotChecked(declProg(
        "  public property R: int => (case Mk() of _ => 1); ",
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 100 do "
        "      var h := new H(); "
        "      total := total + h.R; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(total); "
        "    return 0; "));
    expectNoLeak(r, "100\n");
}

// A PROPERTY ACCESSOR'S EXPRESSION BODY -- the block-form property's
// `get => (case Mk() of _ => 1);` accessor (§23.1). 100 reads amplify the
// leak 100-fold.
BEZEL_TEST(case_expr_owned_scrutinee_capture_8031,
           property_accessor_expression_body_captures_owned_scrutinee_spec_28_3_11) {
    JitResult r = runAotChecked(declProg(
        "  public property R: int "
        "    get => (case Mk() of _ => 1); "
        "  end R; ",
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 100 do "
        "      var h := new H(); "
        "      total := total + h.R; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(total); "
        "    return 0; "));
    expectNoLeak(r, "100\n");
}

// A PROPERTY ACCESSOR'S BLOCK BODY -- the same accessor in its block form
// (`get => do return (case …); end;`). The accessor's body is a
// declaration-site STATEMENT LIST, not part of any method body. 100 reads
// amplify the leak 100-fold.
BEZEL_TEST(case_expr_owned_scrutinee_capture_8031,
           property_accessor_block_body_captures_owned_scrutinee_spec_28_3_11) {
    JitResult r = runAotChecked(declProg(
        "  public property R: int "
        "    get => do "
        "      return (case Mk() of _ => 1); "
        "    end; "
        "  end R; ",
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 100 do "
        "      var h := new H(); "
        "      total := total + h.R; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(total); "
        "    return 0; "));
    expectNoLeak(r, "100\n");
}

// A `requires` CLAUSE (§22.16) -- the clause's expression is evaluated at
// every call, so 100 calls amplify the leak 100-fold.
BEZEL_TEST(case_expr_owned_scrutinee_capture_8031,
           requires_clause_captures_owned_scrutinee_spec_28_3_11) {
    JitResult r = runAotChecked(declProg(
        "  public static method F(x: int): int "
        "    requires ((case Mk() of _ => 1) = 1) "
        "    return x; "
        "  end F; ",
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 100 do "
        "      total := total + F(1); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(total); "
        "    return 0; "));
    expectNoLeak(r, "100\n");
}

// An `ensures` CLAUSE (§22.16) -- the same position as the `requires` clause,
// evaluated at the callee's normal-completion path. (The AOT-checked leg runs
// with §22.16's checked-contracts mode on, so the clause IS emitted.)
BEZEL_TEST(case_expr_owned_scrutinee_capture_8031,
           ensures_clause_captures_owned_scrutinee_spec_28_3_11) {
    JitResult r = runAotChecked(declProg(
        "  public static method F(x: int): int "
        "    ensures ((case Mk() of _ => 1) = 1) "
        "    return x; "
        "  end F; ",
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 100 do "
        "      total := total + F(1); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(total); "
        "    return 0; "));
    expectNoLeak(r, "100\n");
}

// A CLASS `invariant` (§22.16) -- the invariant's expression is evaluated at
// every construction exit, so 100 constructions amplify the leak 100-fold.
BEZEL_TEST(case_expr_owned_scrutinee_capture_8031,
           class_invariant_captures_owned_scrutinee_spec_28_3_11) {
    JitResult r = runAotChecked(declProg(
        "  invariant ((case Mk() of _ => 1) = 1); ",
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 100 do "
        "      var h := new H(); "
        "      total := total + 1; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(total); "
        "    return 0; "));
    expectNoLeak(r, "100\n");
}

// =========================================================================
// The NESTED-EXPRESSION positions -- a `case` inside ANOTHER expression.
//
// The §35.5 arm-borrow-source collector (`collectCaseArmBorrowSourcesExpr`,
// the ownership pass's ONE syntactic walk over a method body) had arms for
// seventeen expression kinds -- `MemberAccessExpr`, `NullCondMemberExpr`,
// `CallExpr`, `NullCondCallExpr`, `IndexExpr`, `BinaryExpr`, `UnaryExpr`,
// `PostfixBangExpr`, ... -- but NOT for `ast::RangeExpr` and NOT for
// `ast::WithExpr`. The read walk (`checkRead`) has BOTH of those arms, which
// is why a `case` nested in a range or a `with(...)` was already reached in a
// METHOD BODY -- and why the DECLARATION-SITE reach (which drives the
// collector) was not.
//
// Every row below asserts the POSITIVE observable: the program's exact
// stdout, its exit code, and `leakedAllocations == 0` /
// `leakedByteCount == 0`.
// =========================================================================

// A `case` NESTED IN A RANGE'S ENDPOINT, at an INSTANCE FIELD INITIALIZER --
// `public r: Range<int> := 1..(case Mk() of _ => 2);`. The initializer runs
// once per constructed instance, so 100 constructions amplify the leak
// 100-fold.
BEZEL_TEST(case_expr_owned_scrutinee_capture_8031,
           field_initializer_case_in_range_captures_owned_scrutinee_spec_28_3_11) {
    JitResult r = runAotChecked(nestedProg(
        "  public r: Range<int> := 1..(case Mk() of _ => 2); ",
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 100 do "
        "      var h := new H(); "
        "      total := total + (h.r.End ?? 0); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(total); "
        "    return 0; "));
    expectNoLeak(r, "200\n");
}

// A `case` NESTED IN A RANGE'S ENDPOINT, at a STATIC FIELD INITIALIZER --
// `public static readonly r: Range<int> := 1..(case Mk() of _ => 2);`. The
// static initializer runs ONCE per program (§22.13's static-init model), so
// this is a single-shot row: the pre-fix compiler reads 3 allocations / 224
// bytes here.
BEZEL_TEST(case_expr_owned_scrutinee_capture_8031,
           static_field_initializer_case_in_range_captures_owned_scrutinee_spec_28_3_11) {
    JitResult r = runAotChecked(nestedProg(
        "  public static readonly r: Range<int> := 1..(case Mk() of _ => 2); ",
        "    Console.WriteLine(H.r.End ?? 0); "
        "    return 0; "));
    expectNoLeak(r, "2\n");
}

// A `case` NESTED IN A RECORD `with(...)` OVERRIDE VALUE, at an INSTANCE FIELD
// INITIALIZER -- `public r: R2 := new R2() with (v := (case Mk() of _ => 1));`.
// The initializer runs once per constructed instance, so 100 constructions
// amplify the leak 100-fold.
BEZEL_TEST(case_expr_owned_scrutinee_capture_8031,
           field_initializer_case_in_record_with_captures_owned_scrutinee_spec_28_3_11) {
    JitResult r = runAotChecked(nestedProg(
        "  public r: R2 := new R2() with (v := (case Mk() of _ => 1)); ",
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 100 do "
        "      var h := new H(); "
        "      total := total + h.r.v; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(total); "
        "    return 0; "));
    expectNoLeak(r, "100\n");
}

// A `case` NESTED IN A RECORD `with(...)` OVERRIDE VALUE, at a STATIC FIELD
// INITIALIZER -- `public static readonly r: R2 := new R2() with (v := (case
// Mk() of _ => 1));`. The static initializer runs ONCE per program, so this
// is a single-shot row: the pre-fix compiler reads 3 allocations / 224 bytes
// here.
BEZEL_TEST(case_expr_owned_scrutinee_capture_8031,
           static_field_initializer_case_in_record_with_captures_owned_scrutinee_spec_28_3_11) {
    JitResult r = runAotChecked(nestedProg(
        "  public static readonly r: R2 := new R2() with (v := (case Mk() of _ => 1)); ",
        "    Console.WriteLine(H.r.v); "
        "    return 0; "));
    expectNoLeak(r, "1\n");
}

// A `case` NESTED IN A RANGE'S ENDPOINT, in a METHOD BODY -- the local-decl
// initializer `var r: Range<int> := 1..(case Mk() of _ => 2);`. This position
// is reached by the READ walk (`checkRead`'s own `RangeExpr` arm), so it is
// the METHOD-BODY twin of the field-initializer row above; 100 evaluations
// amplify the leak 100-fold.
BEZEL_TEST(case_expr_owned_scrutinee_capture_8031,
           body_case_in_range_captures_owned_scrutinee_spec_28_3_11) {
    JitResult r = runAotChecked(nestedBodyProg(
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 100 do "
        "      var r: Range<int> := 1..(case Mk() of _ => 2); "
        "      total := total + (r.End ?? 0); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(total); "
        "    return 0; "));
    expectNoLeak(r, "200\n");
}

// A `case` NESTED IN A RECORD `with(...)` OVERRIDE VALUE, in a METHOD BODY --
// the local-decl initializer `var r := new R2() with (v := (case Mk() of _ =>
// 1));`. This position is reached by the READ walk (`checkRead`'s own
// `WithExpr` arm), so it is the METHOD-BODY twin of the field-initializer row
// above; 100 evaluations amplify the leak 100-fold.
BEZEL_TEST(case_expr_owned_scrutinee_capture_8031,
           body_case_in_record_with_captures_owned_scrutinee_spec_28_3_11) {
    JitResult r = runAotChecked(nestedBodyProg(
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 100 do "
        "      var r := new R2() with (v := (case Mk() of _ => 1)); "
        "      total := total + r.v; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(total); "
        "    return 0; "));
    expectNoLeak(r, "100\n");
}
