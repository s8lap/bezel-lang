// fix #2819 — an arithmetic/bitwise/shift/ordering operator with a UNIT (`()`)
// operand is ill-formed (§3.5 *Operators on primitive types*, §6.2 *Mixed-type
// arithmetic*, §5.1 *Implicit conversions*, §4.7 *The unit type*, §30.3, v0.240).
//
// THE RULE, established from the documents before the check was written:
//
//   * §3.5 enumerates each intrinsic operator group BY OPERAND TYPE —
//     "**Arithmetic** on `byte`, `sbyte`, `short`, `ushort`, `int`, `uint`,
//     `long`, `ulong`, `float`, `double`, `decimal`: `+`, `-`, `*`, `/`,
//     unary `-`, and unary `+`", "**Bitwise** on `byte`, … `ulong`",
//     "**Shift** on the integer types", "**Ordering** (`<`, `<=`, `>`, `>=`) on
//     every primitive **except `bool`**". `()` appears in none of them, and is
//     not a primitive at all (§3.1 lists the unit type separately). §3.5 then
//     closes the door: "The set of admitted combinations is fixed; primitives
//     do not declare cross-type `operator +` methods that user code could
//     inspect."
//   * §6.2 gives the disposition for an operand pair with no shared type: "If
//     no implicit conversion exists between the operand types (e.g. `double`
//     and `decimal`), the operation is a compile error and an explicit cast is
//     required."
//   * §5.1's conversion table has no row out of `()` to any numeric type; its
//     only conversion out of unit is "any type to `Object`" by boxing.
//   * §25.2 draws the candidate set for non-primitive operands from `operator`
//     overloads on the operand types; the unit type declares none and cannot be
//     extended to (§26.2).
//
// So the documents settle it: ill-formed, and a compile error. (Recorded here
// because the brief required establishing it before adding a rejection.)
//
// WHAT WENT WRONG. Before the fix the expression was ACCEPTED, with three
// different wrong outcomes depending only on where the unit came from:
//   `total + t.Wait()`  -> compiled, contributed 0 every iteration (#2819's
//                          repro — and the reason #2807's original report
//                          carried a false exit-code claim nobody contradicted)
//   `total + C.Nop()`   -> malformed IR: an `sadd.with.overflow` fed a `void`
//                          ("Call parameter type does not match function
//                          signature") — a codegen abort, not a diagnostic
//   `total + u`         -> `()`-typed local; degraded to 0 via the #2434 note
// One clean rejection replaces all three.
//
// TWO PARTS TO THE FIX, both needed:
//   (1) ExprTypecheck_Exprs.cpp: reject a `Kind::Unit` operand in typeBinary
//       (arithmetic/bitwise/shift/ordering) and typeUnary (`+`/`-`/`~`).
//   (2) ExprTypecheck_Calls.cpp: `Task.Wait()` is declared `public method
//       Wait();` (§30.3) — no return type, so §4.7 makes its result unit — but
//       sema left the CallExpr with NO exprType at all. Null, not `Kind::Unit`.
//       The gate deliberately does NOT key on "untyped" (that means "sema could
//       not type this", reachable from many unrelated shapes, and would be a
//       broad false reject), so `Wait()` had to be given the type §30.3 assigns
//       it. Without (2) the issue's own repro still compiles.
//
// ⚠ This is a REJECTION, so the false-reject hazard dominates. T4/T5 below are
// the accept-controls, one per legitimate use of a unit-valued expression; they
// outnumber the reject cases on purpose.
//
// Assertions are on DIAGNOSTIC TEXT via the sema-only pipeline (no process is
// ever run), so a rejection can never be confused with a runtime crash the way
// a coarse `rejects()` helper would allow.
//
// TEST MATRIX:
//   T1  the issue's repro — `total := total + t.Wait();` is rejected, naming
//       the unit operand. Plus the two other unit SOURCES (a void-returning
//       call, a `()`-typed local) — category-blindness, as #2819 asked.
//   T2  the whole rejected operator set: `+ - * / mod & | ^ << >>` and the
//       ordering `< > <= >=`, unit on the left and on the right.
//   T3  unary `+`, `-`, `~` on a unit operand.
//   T4  ACCEPT-CONTROLS (sema-clean): bare `t.Wait();`, a unit-returning method
//       called for effect, `_ := f();`, `var u: () := ();`, `u := ();`,
//       `var o: Object := ();` (§8.6 boxing of the unit value), `$"{u}"`
//       interpolation, a lambda whose body is a unit call, and `()?`.
//   T5  ACCEPT-CONTROL (runs, asserts a VALUE): every legitimate `Wait()` /
//       `Result` shape still compiles and computes the same answer — the
//       blast-radius check for part (2) of the fix.
//   T6  `u = v` on two unit operands stays ACCEPTED — §8.4 defines `=` as
//       `Equals` and unit reaches `Object` (§8.6; §8.5 even fixes its
//       `ToString` as `"()"`). No spec text withdraws equality from unit, so
//       rejecting it would be a false reject with no authority. Pins that the
//       new gate does not over-fire onto `=`/`<>`.

#include <string>
#include <string_view>

#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCollect;

namespace {

// Parse + collect + resolve + typecheck. Sema only — nothing is compiled or
// run, so an assertion on the diagnostic text cannot be satisfied by a crash.
bezel::test::SemaResult semaAll2819(std::string_view src) {
    auto r = parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some diagnostic's message contains `needle`.
bool hasDiag2819(const bezel::test::SemaResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// The exact wording the fix emits. Asserting the TEXT (not merely "an error
// occurred") is what makes this test a contract on the diagnostic rather than
// on "something went wrong somewhere".
constexpr std::string_view kUnitBinary =
    "is not defined for a unit (`()`) operand";
constexpr std::string_view kUnitUnary =
    "unary operator `";

// A program whose Main body is `body`, with the helpers the unit shapes need.
std::string prog2819(std::string_view body) {
    return std::string(
        "module M; "
        "class C "
        "  private static method Nop() "
        "  end Nop; "
        "  private static async method Work(i: int): Task<int> "
        "    return i * 2; "
        "  end Work; "
        "  public static method Main(): int ") +
        std::string(body) +
        "  end Main; "
        "end C;";
}

}  // namespace

// ===========================================================================
// T1 — the issue's repro, plus the other two unit SOURCES. Category-blind: the
// gate keys on the operand's resolved type being `Kind::Unit`, not on which
// expression produced it.
// ===========================================================================
BEZEL_TEST(unit_operand_arithmetic_2819, t1_wait_operand_rejected_3_5_30_3) {
    // The exact program from #2819: `Task<T>.Wait()` lowers to unit (§30.3),
    // so `total + t.Wait()` is ill-formed. It used to compile and read 0.
    auto r = semaAll2819(prog2819(
        "    var total := 0; "
        "    for var i in 1..20 do "
        "      using var t := C.Work(i); "
        "      total := total + t.Wait(); "
        "    end; "
        "    return total; "));
    BEZEL_EXPECT_TRUE(hasDiag2819(r, kUnitBinary));
}

BEZEL_TEST(unit_operand_arithmetic_2819, t1b_void_call_operand_rejected_3_5) {
    // A method declared without a return type returns unit (§4.7). This shape
    // used to emit malformed IR rather than a diagnostic.
    auto r = semaAll2819(prog2819(
        "    var total := 0; "
        "    total := total + C.Nop(); "
        "    return total; "));
    BEZEL_EXPECT_TRUE(hasDiag2819(r, kUnitBinary));
}

BEZEL_TEST(unit_operand_arithmetic_2819, t1c_unit_local_operand_rejected_4_7) {
    // A `()`-typed local — the third source. Used to degrade to 0 (#2434).
    auto r = semaAll2819(prog2819(
        "    var u: () := (); "
        "    var total := 0; "
        "    total := total + u; "
        "    return total; "));
    BEZEL_EXPECT_TRUE(hasDiag2819(r, kUnitBinary));
}

// ===========================================================================
// T2 — the whole rejected binary operator set, unit on BOTH sides. §3.5 defines
// arithmetic, bitwise, shift and ordering by operand type and admits `()` in
// none of them.
// ===========================================================================
BEZEL_TEST(unit_operand_arithmetic_2819, t2_all_binary_operators_rejected_3_5) {
    // Arithmetic, bitwise and shift: unit on the RIGHT.
    for (std::string_view op : {"+", "-", "*", "/", "mod", "&", "|", "^",
                                "<<", ">>"}) {
        auto r = semaAll2819(prog2819(
            std::string("    var u: () := (); return 5 ") + std::string(op) +
            " u; "));
        BEZEL_EXPECT_TRUE(hasDiag2819(r, kUnitBinary));
    }
    // ...and on the LEFT (the gate must not be keyed to one side).
    for (std::string_view op : {"+", "-", "*", "/", "mod", "&", "|", "^",
                                "<<", ">>"}) {
        auto r = semaAll2819(prog2819(
            std::string("    var u: () := (); return u ") + std::string(op) +
            " 5; "));
        BEZEL_EXPECT_TRUE(hasDiag2819(r, kUnitBinary));
    }
    // Ordering (§3.5 "Ordering ... on every primitive except `bool`").
    for (std::string_view op : {"<", ">", "<=", ">="}) {
        auto r = semaAll2819(prog2819(
            std::string("    var u: () := (); if u ") + std::string(op) +
            " 5 then return 1; end; return 0; "));
        BEZEL_EXPECT_TRUE(hasDiag2819(r, kUnitBinary));
    }
}

// ===========================================================================
// T3 — unary `+`, `-`, `~`. §3.5 groups unary `+`/`-` under Arithmetic and `~`
// under Bitwise, both enumerated by operand type. The pre-existing bad-kind
// checks for these operators (#1574/#1588/§6.5) all gate on `Kind::Primitive`,
// so a Unit operand slipped past every one and fell through to the identity
// pass-through: `-u` read 0, `~u` read -1.
// ===========================================================================
BEZEL_TEST(unit_operand_arithmetic_2819, t3_unary_operators_rejected_3_5) {
    for (std::string_view op : {"-", "~"}) {
        auto r = semaAll2819(prog2819(
            std::string("    var u: () := (); return ") + std::string(op) +
            "u; "));
        BEZEL_EXPECT_TRUE(hasDiag2819(r, kUnitUnary));
    }
    // Unary `+` (the §3.5 identity) — same rule.
    auto rp = semaAll2819(prog2819("    var u: () := (); var v := +u; return 0; "));
    BEZEL_EXPECT_TRUE(hasDiag2819(rp, kUnitUnary));
}

// ===========================================================================
// T4 — ACCEPT-CONTROLS. Every legitimate use of a unit-valued expression must
// stay sema-clean of the new diagnostic. A false reject here breaks working
// code, which is worse than the bug the rejection fixes.
// ===========================================================================
BEZEL_TEST(unit_operand_arithmetic_2819, t4_legitimate_unit_uses_accepted) {
    struct Case { const char* name; const char* body; };
    const Case cases[] = {
        // A bare `Wait()` STATEMENT — the correct spelling of the issue's code.
        {"bare_wait",
         "    using var t := C.Work(3); t.Wait(); return 0; "},
        // A unit-returning method called for effect.
        {"bare_void_call",
         "    C.Nop(); return 0; "},
        // §17.13 discard.
        {"discard",
         "    _ := C.Nop(); return 0; "},
        // §4.7: a `()`-typed local, declared and assigned.
        {"unit_local",
         "    var u: () := (); return 0; "},
        {"unit_assign",
         "    var u: () := (); u := (); return 0; "},
        // §8.6 *Boxing of the unit value*: "`var o: Object := ();` is
        // grammatically valid and produces a boxed `()`". A boxed unit's static
        // type is `Object`, not `()`, so it never reaches the new gate — which
        // is exactly why boxing had to be checked rather than assumed.
        {"unit_boxed_to_object",
         "    var o: Object := (); return 0; "},
        // §8.5 fixes unit's `ToString` as `"()"`, so interpolation is defined.
        {"unit_interpolated",
         "    var u: () := (); var s := $\"u={u}\"; return s.Length; "},
        // §27.3: a lambda whose body is a unit call.
        {"lambda_unit_body",
         "    var f := lambda() => C.Nop(); f(); return 0; "},
        // §4.7: "Wrapping the unit type with `?` ... is permitted by the
        // grammar and produces an optional with two values, `null` and `()`."
        {"nullable_unit",
         "    var u: ()? := (); if u <> null then return 0; end; return 1; "},
    };
    for (const auto& c : cases) {
        auto r = semaAll2819(prog2819(c.body));
        // Neither the binary nor the unary unit diagnostic may fire.
        bool falseReject = hasDiag2819(r, kUnitBinary) ||
                           hasDiag2819(r, "is not defined for a unit");
        if (falseReject) {
            std::printf("    FALSE REJECT on accept-control '%s'\n", c.name);
        }
        BEZEL_EXPECT_TRUE(!falseReject);
    }
}

// ===========================================================================
// T5 — ACCEPT-CONTROL asserting a VALUE, not merely a clean compile. Part (2)
// of the fix gives `Task.Wait()` an exprType it never had; this pins that every
// legitimate `Wait()`/`Result` shape still compiles AND computes the same
// answer. 2*20 + 2 == 42.
// ===========================================================================
BEZEL_TEST(unit_operand_arithmetic_2819, t5_wait_and_result_still_work_30_3) {
    auto r = bezel::test::runAot(
        "module M; "
        "class C "
        "  private static async method Work(i: int): Task<int> "
        "    return i * 2; "
        "  end Work; "
        "  public static method Main(): int "
        "    using var t := C.Work(20); "
        "    t.Wait(); "                       // unit, used as a statement
        "    return t.Result + 2; "            // the value comes from Result
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

BEZEL_TEST(unit_operand_arithmetic_2819, t5b_wait_in_loop_accumulates_30_3) {
    // The SHAPE #2819's repro was reaching for, spelled correctly: `Wait()` as
    // a statement, the value read from `Result`. 2+4+6 == 12.
    auto r = bezel::test::runAot(
        "module M; "
        "class C "
        "  private static async method Work(i: int): Task<int> "
        "    return i * 2; "
        "  end Work; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..3 do "
        "      using var t := C.Work(i); "
        "      t.Wait(); "
        "      total := total + t.Result; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 12);
}

// ===========================================================================
// T6 — `=`/`<>` on unit operands stays ACCEPTED. §8.4: "`a = b` invokes
// `a.Equals(b)`, with a null-safe wrapper"; unit reaches `Object` by boxing
// (§8.6) and §8.5 specifies its `ToString`. No spec text withdraws equality
// from unit, so the new gate must not extend to the equality group — this pins
// the boundary of the rejection.
// ===========================================================================
BEZEL_TEST(unit_operand_arithmetic_2819, t6_unit_equality_not_rejected_8_4) {
    for (std::string_view op : {"=", "<>"}) {
        auto r = semaAll2819(prog2819(
            std::string("    var u: () := (); var v: () := (); if u ") +
            std::string(op) + " v then return 0; end; return 1; "));
        BEZEL_EXPECT_TRUE(!hasDiag2819(r, "is not defined for a unit"));
    }
}
