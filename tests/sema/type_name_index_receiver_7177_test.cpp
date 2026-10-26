// fix #7177 (§23.4 / §23.5 / §27.1 / §27.6 / §27.7, v0.344) — an INDEX on
// a TYPE NAME was ACCEPTED, and the program it produced printed NOTHING (or
// a WRONG value).
//
// THE RULE. §23.4 (v0.344): "An indexer is a property whose name is replaced
// by `[parameters]`" — an index expression `a[i]` reads a DECLARED indexer of
// the receiver, and §23.4's own sentence ("Inside a member of a class with
// its own indexer, `this[i]` is required (the brackets need a left-hand
// side)") says the brackets need a LEFT-HAND SIDE. §23.5 (v0.344) adds the
// sentence that decides this issue: "Static indexers are not supported." An
// indexer is therefore an INSTANCE member, and the receiver of an index must
// be a VALUE. §2.7 (v0.344) resolves a dotted chain against the TYPE its
// head names (a nested type, an enum constant or a static member): a type
// name in receiver position is therefore legal — §22.12's static-member
// form spells it (`Registry<int>.Register()`) — but only for MEMBER access,
// never for an index.
//
// WHAT WENT WRONG. The IndexExpr typing chain (ExprTypecheck_Exprs.cpp)
// keys on the receiver's resolved TYPE. A type name in expression position
// carries no value type (`Plain` resolves to a TypeSymbol; `Box<int>` to an
// instantiated TypeRef), so the chain either skipped the receiver entirely
// (`Plain[0]`, `PlainNoIdx[0]`, `Array<int>[0]`) or resolved an indexer
// against the TYPE rather than against an instance (`Box<int>[0]` over a
// `Box<T>` that DECLARES an indexer). Either way the IndexExpr was left
// untyped and codegen degraded it.
//
// MEASURED on the pre-fix base (r30-c1 tip 40d1691f), one compile + one run
// per row:
//   * `Console.WriteLine(Plain[0]);` — `Plain` a non-generic class that
//     DECLARES `property [i: int]: int` — compiled, ran, exited 0 and
//     printed NOTHING (one #3079 internal-degradation warning).
//   * `var z: int := Plain[0];` — compiled, ran and printed `0`: a WRONG
//     VALUE, not merely an empty output.
//   * `Plain[0] := 5;` — compiled with NO diagnostic at all.
//   * `Box<int>[0]` — the GENERIC spelling, over a `Box<T>` that DECLARES an
//     indexer — behaved IDENTICALLY to the non-generic one.
//   * `BoxNoIdx<int>[0]` — the generic spelling over a class that declares NO
//     indexer — was REJECTED with the existing §23.4 message ("type `X` does
//     not declare an indexer — `a[i]` requires an indexer (§23.4)").
//
// ⇒⇒ THE ISSUE'S OWN CONTRAST WAS AN ARTEFACT. The issue reported "the
// GENERIC spelling is rejected, the NON-GENERIC one is accepted" — but the
// two probes it used declared DIFFERENT indexers. The verdict tracked only
// whether the type DECLARED an indexer, never whether it was generic. The
// same shape with a type that declares no indexer was rejected (with the
// existing message); the same shape with a type that declares one was
// accepted and silently did nothing.
//
// THE FIX rejects the shape, at the receiver, in one place:
//   * a type name that DECLARES no indexer → the EXISTING §23.4 message,
//     byte for byte, so the generic spelling's diagnostic (which the issue's
//     reviewers pinned) is UNCHANGED and the non-generic spelling now draws
//     the SAME text;
//   * a type name that DOES declare one — or a BUILT-IN class, whose member
//     table the compiler does not carry — → a new message naming the
//     type-name receiver, because "does not declare an indexer" would be a
//     falsehood there.
//
// The parallel §7.5 rejection (a `?.` on a type name, #5452) is the same
// rule for the null-conditional operator, and it already covers both the
// generic and the non-generic spelling.
//
// A SECOND, DISTINCT family is closed by the same change: a §27.6 METHOD
// REFERENCE is a FUNCTION value, and a function type declares no members of
// its own — so `h.Id<int>.Get()`, `h.Id<int>?.Get()`, `h.Id<int>[0]` and their
// null-conditional twins named nothing and were accepted silently. The
// member-access forms are closed by the §27.1 arm of
// `closedValueSurfaceMemberReason` (ExprTypecheck_Calls.cpp); the index
// forms by the §27.1 arm of the IndexExpr gate. The universal §8.1 members
// stay admissible on a function value — `h.Id<int>.ToString()` prints the
// function's signature, MEASURED on the same base.
//
// THAT ARM MUST NOT OVER-REACH, AND T2b PINS THE TWO NAMES IT MAY NOT
// REJECT.  (a) `Dispose()` — §28.9 leaves a function-typed value's
// `Dispose()` ambiguous ("a no-op (or, for types that are not classes and
// therefore have no `Dispose()` at all…)") and the compiler implements it as
// a REAL dispose on a closure cell (§28.2's checked-disposal guard, #4475);
// the banked `closure_dispose_then_invoke_4475` rows pin that runtime half,
// so the spelling stays ADMITTED.  (b) A member an `extension Object`
// provides — §8.1 puts `Object` in a function type's base chain and §26.5
// rule 2 searches the extension blocks on "x's type, base types, or
// implemented interfaces", so the extension REACHES a function-typed
// receiver and the member is not nameless.
//
// TEST MATRIX:
//   T1  the index-on-a-type-name family: each spelling is REJECTED with the
//       diagnostic it is entitled to, at a REAL location, and demonstrably
//       NOT the pre-fix symptom (an empty output, a zero value, or the
//       #3079/#2434 internal-degradation notices).
//   T2  the method-reference family: a member access and an index on a
//       FUNCTION-typed receiver are REJECTED. (The null-conditional-receiver
//       twins are NOT rows here — their receiver is an UNTYPED bare `?.`
//       method access, a different defect; see the note at the end of T2.)
//   T2b the two carve-outs: `Dispose()` on a function value and a member an
//       `extension Object` provides both stay ADMITTED.
//   T3  ACCEPT-CONTROLS that RUN and assert real values — every indexable
//       and every §22.12-legal receiver family stays admitted.
//   T4  the OVER-REJECTION guard: a declared generic PARAMETER whose name
//       matches a type in scope is the DECLARATION's own type parameter
//       (§14.2), not the type, and must not be rejected by this rule.
//
// THE ORACLE. Every row drives the REAL driver (`runAot` — a forked
// `Driver::compileFile`, a clang link, and a native execution), never the
// deprecated in-process JIT. The ACCEPT-CONTROL rows use `runAot` and not
// `runAotChecked`, and that is deliberate: the #3733 toolchain-SKIP ratchet
// (`scripts/check_toolchain_skip_surface.py`) counts every
// `runAotChecked*` call site and REFUSES a rise in that count unless
// `absorbing_sites` STRICTLY falls in the same change — and a lane may not
// bank the baseline (`--update` is the coordinator's). These rows assert
// that a legal program still COMPILES, RUNS and produces a real value; the
// disposal mode is not what they are about, and a checked-oracle call site
// here would leave the gate's ratchet RED with no way for the coordinator to
// absorb it.

#include <string>
#include <string_view>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot / runAotOut
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

constexpr std::string_view kSec234 = "\xC2\xA7" "23.4";
constexpr std::string_view kSec235 = "\xC2\xA7" "23.5";
constexpr std::string_view kSec271 = "\xC2\xA7" "27.1";

// The issue's own program shape, with the three classes the matrix needs:
// `Plain` DECLARES an indexer, `PlainNoIdx` does not, `Box<T>` is the
// generic spelling and declares one.
std::string prog7177(std::string_view body) {
    return std::string(
               "module M; "
               "public class Plain "
               "  private _v: int; "
               "  public constructor(v: int) _v := v; end constructor; "
               "  public property [i: int]: int => _v; "
               "end Plain; "
               "public class PlainNoIdx "
               "  private _v: int; "
               "  public constructor(v: int) _v := v; end constructor; "
               "  public property V: int => _v; "
               "end PlainNoIdx; "
               "public class Box<T> "
               "  private _v: T; "
               "  public constructor(v: T) _v := v; end constructor; "
               "  public property [i: int]: T => _v; "
               "end Box; "
               "public class BoxNoIdx<T> "
               "  private _v: T; "
               "  public constructor(v: T) _v := v; end constructor; "
               "  public property V: T => _v; "
               "end BoxNoIdx; "
               "public class Holder "
               "  public method Id<T>(x: T): T => x; "
               "end Holder; "
               "public class C "
               "  public static method Main(): int ") +
           std::string(body) + " return 0; end Main; end C;";
}

// A driver-level rejection row: the whole `bezelc` pipeline must refuse the
// program, name THIS diagnostic, land on a REAL source location, and must
// NOT show any of the pre-fix symptoms (an empty output, a zero value, or
// the #3079 / #2434 internal-degradation notices).
void expectRejectedWith(const std::string& program, const char* needle) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
    // NOT the pre-fix symptoms.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "#3079"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "#2434"));
    BEZEL_EXPECT_FALSE(
        diagnosticMessageContains(r, "failed to lower to a value"));
    BEZEL_EXPECT_FALSE(
        diagnosticMessageContains(r, "degraded to a zero-initialized value"));
    // NOT a crash, NOT a codegen failure, NOT a signal or a timeout.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "terminated by signal"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "Assertion"));
}

// The type-name receiver message the fix introduces.
const char* typeNameIndexNeedle(const char* typeName) {
    static thread_local std::string buf;
    buf = std::string("the index operator `[]` cannot be applied to a type "
                       "name (`") +
          typeName + "`)";
    return buf.c_str();
}

}  // namespace

// ===========================================================================
// T1 — an index on a TYPE NAME is rejected.
// ===========================================================================

// The issue's EXACT repro: `Plain[0]`, a non-generic class that DECLARES an
// indexer. Pre-fix: exit 0, the #3079 warning, and the program printed
// NOTHING.
BEZEL_TEST(sema_type_name_index_receiver_7177,
           t1_issue_exact_plain_index_rejected_spec_23_4) {
    expectRejectedWith(
        prog7177(" Console.WriteLine(Plain[0]); "),
        typeNameIndexNeedle("Plain"));
    auto r = runAot(prog7177(" Console.WriteLine(Plain[0]); "));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, std::string(kSec234).c_str()));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, std::string(kSec235).c_str()));
}

// The same shape over a class that declares NO indexer: the EXISTING §23.4
// message, byte for byte — the wording the generic spelling already draws.
BEZEL_TEST(sema_type_name_index_receiver_7177,
           t1_plain_without_indexer_keeps_the_existing_23_4_message) {
    expectRejectedWith(
        prog7177(" Console.WriteLine(PlainNoIdx[0]); "),
        "type `PlainNoIdx` does not declare an indexer");
}

// The GENERIC spelling over a class that DECLARES an indexer — the twin the
// issue reported as "rejected". It was accepted too (over a `Box<T>` that
// declares one); it now draws the same type-name message as the non-generic
// spelling.
BEZEL_TEST(sema_type_name_index_receiver_7177,
           t1_generic_type_name_index_rejected_spec_23_4) {
    expectRejectedWith(
        prog7177(" Console.WriteLine(Box<int>[0]); "),
        typeNameIndexNeedle("Box"));
}

// The GENERIC spelling over a class that declares NO indexer: the message
// this issue's reviewers PINNED. It must be UNCHANGED, byte for byte.
BEZEL_TEST(sema_type_name_index_receiver_7177,
           t1_generic_type_name_without_indexer_keeps_the_pinned_message) {
    expectRejectedWith(
        prog7177(" Console.WriteLine(BoxNoIdx<int>[0]); "),
        "type `BoxNoIdx` does not declare an indexer");
    auto r = runAot(prog7177(" Console.WriteLine(BoxNoIdx<int>[0]); "));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, std::string(kSec234).c_str()));
}

// A BUILT-IN class name: the compiler carries no member table for it, so
// "does not declare an indexer" would be a falsehood (stdlib §4.4 declares
// `Array<T>`'s indexer) — the type-name message is the truthful one.
BEZEL_TEST(sema_type_name_index_receiver_7177,
           t1_builtin_type_name_index_rejected_spec_23_4) {
    expectRejectedWith(
        prog7177(" Console.WriteLine(Array<int>[0]); "),
        typeNameIndexNeedle("Array"));
}

// The ASSIGNMENT form. Pre-fix it compiled, ran and printed `0` — a WRONG
// VALUE, which is worse than the empty output.
BEZEL_TEST(sema_type_name_index_receiver_7177,
           t1_assignment_form_rejected_spec_23_4) {
    expectRejectedWith(
        prog7177(" var z: int := Plain[0]; Console.WriteLine(z); "),
        typeNameIndexNeedle("Plain"));
}

// The WRITE form. Pre-fix it compiled with NO diagnostic at all.
BEZEL_TEST(sema_type_name_index_receiver_7177,
           t1_write_form_rejected_spec_23_4) {
    expectRejectedWith(
        prog7177(" Plain[0] := 5; "),
        typeNameIndexNeedle("Plain"));
}

// The issue's EXACT program, whole. runAotOut's "<did-not-run>" sentinel
// proves both halves at once — the program is now REJECTED, so it
// categorically cannot print nothing anymore.
BEZEL_TEST(sema_type_name_index_receiver_7177,
           t1_issue_exact_program_no_longer_runs_spec_23_4) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "public class Plain "
            "  private _v: int; "
            "  public constructor(v: int) _v := v; end constructor; "
            "  public property [i: int]: int => _v; "
            "end Plain; "
            "public class C "
            "  public static method Main(): int "
            "    Console.WriteLine(Plain[0]); "
            "    return 0; "
            "  end Main; "
            "end C;"),
        std::string("<did-not-run>"));
}

// ===========================================================================
// T2 — the METHOD-REFERENCE family. A §27.6 method reference is a FUNCTION
// value; a function type declares no members, so every member access and
// every index on one names nothing.
// ===========================================================================

BEZEL_TEST(sema_type_name_index_receiver_7177,
           t2_method_reference_member_access_rejected_spec_27_1) {
    expectRejectedWith(
        prog7177(" var h := new Holder(); Console.WriteLine(h.Id<int>.Get()); "),
        "no member `Get` on a function value");
}

BEZEL_TEST(sema_type_name_index_receiver_7177,
           t2_method_reference_null_conditional_member_rejected_spec_27_1) {
    expectRejectedWith(
        prog7177(" var h := new Holder(); Console.WriteLine(h.Id<int>?.Get()); "),
        "no member `Get` on a function value");
}

BEZEL_TEST(sema_type_name_index_receiver_7177,
           t2_method_reference_index_rejected_spec_23_4) {
    expectRejectedWith(
        prog7177(" var h := new Holder(); Console.WriteLine(h.Id<int>[0]); "),
        "no indexer on a function value");
}

// ⇒⇒ THE NULL-CONDITIONAL-RECEIVER TWINS ARE NOT ROWS HERE, AND THE REASON
// IS A DIFFERENT DEFECT. `h?.Id<int>.Get()`, `h?.Id<int>?.Get()` and
// `h?.Id<int>[0]` have a receiver that is a BARE `?.` access to a METHOD, and
// sema leaves that node UNTYPED — so the receiver of the `.Get()` / `[0]` is
// not a function value but an UNTYPED expression, and the §27.1 arm is never
// asked. The same hole is reachable with NO explicit type arguments at all
// (`h?.Plain2.ToString()` on a non-generic method is equally untyped and
// equally silent), which is what identifies it as the `?.`-reads-nothing
// family (tracked by #7016 / #6815 / #6793 / #6178 / #7074) rather than a
// member-access-on-a-function-value defect. Closing it means TYPING the bare
// null-conditional method reference (§7.5's nullable result over §27.6's
// formation) in `resolveNullCondMember` — a different change, in a different
// place, on a question the spec does not settle for the BARE `x?.M` spelling
// (it settles the CALL spelling, `user?.Log("Accessed")`, and the nullable
// function value of §7.6). It is reported, not silently widened into this
// lane.
//
// The BASE did reject these three — but as a PARSE error ("did not parse"),
// before #7163's successor-set change routed them into the gap. Nothing that
// used to be rejected became accepted by this change.

// ===========================================================================
// T2b — THE TWO CARVE-OUTS THE FUNCTION-VALUE ARM MUST NOT OVER-REACH.
// The `Kind::Function` arm of `closedValueSurfaceMemberReason` answers "is
// this member name nameless on a function-typed receiver?". Two names are
// NOT nameless, and a rejection of either is a false rejection of a
// spec-admitted program.
// ===========================================================================

// (a) `Dispose()` on a function-typed value. §28.9 (v0.344) says a
// function-typed value's `Dispose()` is "a no-op (or, for types that are not
// classes and therefore have no `Dispose()` at all…)" — the SPELLING is
// contemplated and its EFFECT is left open, and §8.7's "the compiler
// generates `IDisposable` on every non-static class" does not settle a
// non-class. The compiler has always implemented it as a REAL dispose on a
// closure cell (§28.2's checked-disposal guard; the `bz_closure_mark_disposed`
// helper, #4475), and the banked `closure_dispose_then_invoke_4475` rows
// pin that runtime half. So the spelling stays ADMITTED: rejecting it would
// be a silent behaviour flip on a spec-ambiguous point.
//
// This row is a CONTROL, not a fail-without row: the BASE admitted it too.
BEZEL_TEST(sema_type_name_index_receiver_7177,
           t2b_dispose_on_a_function_value_stays_admissible_spec_28_9) {
    auto r = runAot(
        "module M; public class C "
        "  public static method Main(): int "
        "    var c := lambda(): int => 42; "
        "    c.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// (b) A member an `extension Object` provides. §8.1 (v0.344): "function
// types reaching `Object` similarly via their reference-type heap
// representation" — so `Object` IS a base type of a function type — and
// §26.5 rule 2 searches the extension blocks "on `x`'s type, base types, or
// implemented interfaces". The extension therefore REACHES a function-typed
// receiver, and §28.3 admits a CAPTURE-FREE function value at the
// set-erasing receiver position of such a call.
//
// MEASURED on the pre-fix base (r30-c1 tip 40d1691f): this program COMPILED
// and printed NOTHING (one #3079 internal-degradation warning) — the
// extension never matched, the member access stayed untyped, and the call
// degraded. ⇒ THIS ROW IS A FAIL-WITHOUT ROW: the base prints nothing, the
// fix prints `7`.
BEZEL_TEST(sema_type_name_index_receiver_7177,
           t2b_extension_on_object_reaches_a_function_receiver_spec_26_5) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "public extension Object "
            "  public method Tag(): int => 7; "
            "end extension; "
            "public class C "
            "  public static method Main(): int "
            "    var f: () -> int := C.One; "
            "    Console.WriteLine(f.Tag()); "
            "    return 0; "
            "  end Main; "
            "  public static method One(): int => 1; "
            "end C;"),
        std::string("7"));
}

// ===========================================================================
// T3 — ACCEPT-CONTROLS. Every indexable receiver family and every §22.12
// legal type-name spelling stays admitted, each RUNS and asserts a real
// value. A rejection that broke working code would be worse than the bug.
// ===========================================================================

// A user class's OWN indexer (§23.4) — the receiver is an INSTANCE.
BEZEL_TEST(sema_type_name_index_receiver_7177,
           t3_user_indexer_on_an_instance_still_works_spec_23_4) {
    auto r = runAot(
        "module M; "
        "public class Plain "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public property [i: int]: int => _v; "
        "end Plain; "
        "public class C "
        "  public static method Main(): int "
        "    using var p := new Plain(7); "
        "    return p[0]; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// The GENERIC spelling of the same: an index on a generic VALUE.
BEZEL_TEST(sema_type_name_index_receiver_7177,
           t3_generic_value_index_still_works_spec_23_4) {
    auto r = runAot(
        "module M; "
        "public class Box<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public property [i: int]: T => _v; "
        "end Box; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Box<int>(9); "
        "    return b[0]; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 9);
}

// §22.12's static-member form on a GENERIC type name — the legal spelling
// that sits one token away from the rejected one.
BEZEL_TEST(sema_type_name_index_receiver_7177,
           t3_static_member_on_a_generic_type_name_still_works_spec_22_12) {
    auto r = runAot(
        "module M; "
        "public class Box<T> "
        "  public static method Make(): int => 5; "
        "end Box; "
        "public class C "
        "  public static method Main(): int "
        "    return Box<int>.Make(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// The same on a NON-generic type name.
BEZEL_TEST(sema_type_name_index_receiver_7177,
           t3_static_member_on_a_plain_type_name_still_works_spec_22_12) {
    auto r = runAot(
        "module M; "
        "public class Plain "
        "  public static method Make(): int => 6; "
        "end Plain; "
        "public class C "
        "  public static method Main(): int "
        "    return Plain.Make(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);
}

// An explicit type-argument list on a legal generic member (§14.1) — the
// §27.6 method-reference form, still legal when the reference is FORMED and
// CALLED.
BEZEL_TEST(sema_type_name_index_receiver_7177,
           t3_method_reference_formed_and_called_still_works_spec_27_6) {
    auto r = runAot(
        "module M; "
        "public class Holder "
        "  public method Id<T>(x: T): T => x; "
        "end Holder; "
        "public class C "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    var f := h.Id<int>; "
        "    return f(3); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// The universal §8.1 members ARE members of a function value — `ToString`
// prints the function's signature. It must keep working.
BEZEL_TEST(sema_type_name_index_receiver_7177,
           t3_universal_member_on_a_function_value_still_works_spec_8_1) {
    auto r = runAot(
        "module M; "
        "public class Holder "
        "  public method Id<T>(x: T): T => x; "
        "end Holder; "
        "public class C "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    Console.WriteLine(h.Id<int>.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("(int) -> int") != std::string::npos);
}

// A `?.` on a legal VALUE receiver (§7.5).
BEZEL_TEST(sema_type_name_index_receiver_7177,
           t3_null_conditional_on_a_value_still_works_spec_7_5) {
    auto r = runAot(
        "module M; "
        "public class PlainNoIdx "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public property V: int => _v; "
        "end PlainNoIdx; "
        "public class C "
        "  public static method Main(): int "
        "    using var p := new PlainNoIdx(8); "
        "    var q: PlainNoIdx? := p; "
        "    Console.WriteLine((q?.V) ?? 0); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("8") != std::string::npos);
}

// ===========================================================================
// T4 — the OVER-REJECTION guard. A declared generic PARAMETER whose name
// matches a type in scope is the DECLARATION's own type parameter (§14.2),
// not the type, and the rule must not be keyed on the SPELLING. The
// parameter carries an indexer of its own, so the access resolves and reads
// the element: 40 + 2 = 42.
// ===========================================================================
BEZEL_TEST(sema_type_name_index_receiver_7177,
           t4_type_parameter_named_plain_is_not_the_type_spec_14_2) {
    const std::string src =
        "module M; "
        "public interface IIndexable readonly property [i: int]: int; "
        "end IIndexable; "
        "public class SingleInt: IIndexable "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public property [i: int]: int => _v + i; "
        "end SingleInt; "
        "public class C "
        "  public static method At<Plain>(x: Plain, i: int): int "
        "    where Plain: IIndexable "
        "    return x[i]; "
        "  end At; "
        "  public static method Main(): int "
        "    using var s := new SingleInt(40); "
        "    return At<SingleInt>(s, 2); "
        "  end Main; "
        "end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}
