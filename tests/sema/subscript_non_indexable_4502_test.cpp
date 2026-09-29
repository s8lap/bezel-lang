// fix #4502 — subscripting a value that declares no indexer (`b[0]` on a
// `bool`, `int`, or `decimal` local, and every other non-`string` primitive)
// compiled with exit 0 and only the #2434 degraded-lowering warning; the
// program RAN. This is the `[...]` analogue of #3153 (a call on a
// non-callable value) and #3155 (`x?.M` never resolving), and the primitive-
// receiver hole #423 left open (#423 fixed only the INTERFACE-receiver form)
// (§23.4, v0.306).
//
// THE RULE. §23.4 (v0.306): "An indexer is a property whose name is replaced
// by `[parameters]`" — an indexer is a DECLARED member. §3.4/§4.6 declare
// exactly one BUILT-IN exception: `string`'s `[i]`/`[range]` form. No other
// primitive type (`bool`, `char`, every numeric type `byte`..`decimal`,
// `Object`) declares an indexer, built-in or otherwise: §26.2's extension
// surface admits only "methods and computed read-only properties" (§26.2
// line "Only methods and computed read-only properties" — no indexer), so no
// primitive can acquire one outside the compiler's own built-ins either.
// `a[i]` on any of them therefore names nothing, mirroring the already-
// correct #2800 diagnostic for a Named-declaration receiver with no indexer
// member ("type `X` does not declare an indexer").
//
// WHAT WENT WRONG. Measured on d3b36b1c (docs v0.306), one compile + one run:
//   `var v := b[0];` (`b: bool`)    -> bezelc exit 0, ONLY the #2434 warning
//                                      on stderr; the program printed `ok`.
//   same shape for `int`, `decimal` -> identical: exit 0, program ran.
// Every branch of the IndexExpr typing chain in ExprTypecheck_Exprs.cpp
// matches on `TypeRef::Kind::InlineArray`, `Primitive`+`String`, or `Named`
// (built-in collection or user declaration) — a non-`string` `Primitive`
// receiver matched NONE of them, so the IndexExpr was left untyped and
// codegen's #2434 backstop degraded the read to a zero-initialized value
// instead of failing the compile.
//
// THE FIX adds exactly one `else if (tt->kind == TypeRef::Kind::Primitive)`
// arm, positioned after the existing `Primitive`+`String` arm (so `string`
// is unaffected) and before the `Named` arms, mirroring the #2800 wording.
//
// TEST MATRIX:
//   T1  the issue's three measured rows, through the REAL driver (`runAot`):
//       rejected, with the specific §23.4 diagnostic naming the primitive's
//       spelling, and demonstrably NOT the #2434 internal warning, a crash,
//       or a signal. Plus one `runAotOut` row pinning that the issue's exact
//       program — which used to print "ok" — no longer runs at all.
//   T2  breadth: every OTHER primitive kind (`char`, `Object`, and the full
//       numeric set) rejected too, sema-only (no driver overhead needed to
//       see the diagnostic).
//   T3  a NULLABLE primitive receiver (`int?`) is rejected with this
//       diagnostic too — nullability is irrelevant to "does the primitive
//       kind declare an indexer at all".
//   T4  ACCEPT-CONTROLS that RUN and assert real values — every indexable
//       receiver family stays admitted: `string` (char index AND range
//       slice), `Array<T>`, `Dictionary<K,V>`, a user class's OWN indexer,
//       and an INTERFACE receiver whose indexer the implementing class
//       provides (the #423 positive case — #423 fixed the reject when the
//       interface has NO indexer; this pins that a real one still works).

#include <string>
#include <string_view>

#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_result_asserts.hpp"   // diagnosticMessageContains
#include "support/aot_support.hpp"          // runAot / runAotChecked / runAotOut
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::parseAndCollect;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

bezel::test::SemaResult semaAll4502(std::string_view src) {
    auto r = parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasDiag4502(const bezel::test::SemaResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

constexpr std::string_view kSec234 = "\xC2\xA7" "23.4";

std::string prog4502(std::string_view body) {
    return std::string("module M; class C public static method Main(): int ") +
           std::string(body) + " end Main; end C;";
}

// A driver-level (AOT) rejection row: the whole `bezelc` pipeline must refuse
// the program, naming this diagnostic, must NOT emit the #2434 degraded-
// lowering warning (the pre-fix symptom), and must not crash.
void expectDriverRejectsSubscript(const std::string& body,
                                  const char* primitiveSpelling) {
    auto r = runAot(prog4502(body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, (std::string("type `") + primitiveSpelling +
            "` does not declare an indexer")
               .c_str()));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, std::string(kSec234).c_str()));
    // NOT the pre-fix symptom: the #2434 internal degraded-lowering notice.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "#2434"));
    BEZEL_EXPECT_FALSE(
        diagnosticMessageContains(r, "degraded to a zero-initialized value"));
    // NOT a crash, NOT a codegen failure, NOT a signal or a timeout.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "terminated by signal"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "Assertion"));
}

}  // namespace

// ===========================================================================
// T1 — the issue's three measured rows, through the real driver.
// ===========================================================================
BEZEL_TEST(sema_subscript_non_indexable_4502,
           t1_bool_subscript_rejected_spec_23_4) {
    // Pre-fix: exit 0, only the #2434 warning, program printed `ok`.
    expectDriverRejectsSubscript(
        " var b: bool := false; var v := b[0]; "
        " Console.WriteLine(\"ok\"); return 0; ",
        "bool");
}

BEZEL_TEST(sema_subscript_non_indexable_4502,
           t1_int_subscript_rejected_spec_23_4) {
    expectDriverRejectsSubscript(
        " var n: int := 5; var v := n[0]; "
        " Console.WriteLine(\"ok\"); return 0; ",
        "int");
}

BEZEL_TEST(sema_subscript_non_indexable_4502,
           t1_decimal_subscript_rejected_spec_23_4) {
    expectDriverRejectsSubscript(
        " var d: decimal := 1.5m; var v := d[0]; "
        " Console.WriteLine(\"ok\"); return 0; ",
        "decimal");
}

// The issue's EXACT program (bool receiver): pre-fix this printed "ok" and
// exited 0. runAotOut's "<did-not-run>" sentinel proves both halves at once —
// the program is now REJECTED, so it categorically cannot print "ok" anymore.
BEZEL_TEST(sema_subscript_non_indexable_4502,
           t1_issue_exact_program_no_longer_runs_spec_23_4) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module Conf; "
            "class W "
            "  public static method Main() "
            "    var b: bool := false; "
            "    var v := b[0]; "
            "    Console.WriteLine(\"ok\"); "
            "  end Main; "
            "end W;"),
        std::string("<did-not-run>"));
}

// ===========================================================================
// T2 — breadth: every OTHER primitive kind is rejected too.
// ===========================================================================
BEZEL_TEST(sema_subscript_non_indexable_4502,
           t2_every_other_primitive_rejected_spec_23_4) {
    struct Row { const char* ty; const char* init; const char* spelling; };
    const Row rows[] = {
        {"char", "'a'", "char"},
        {"Object", "5", "Object"},
        {"byte", "default(byte)", "byte"},
        {"sbyte", "default(sbyte)", "sbyte"},
        {"short", "default(short)", "short"},
        {"ushort", "default(ushort)", "ushort"},
        {"uint", "default(uint)", "uint"},
        {"long", "default(long)", "long"},
        {"ulong", "default(ulong)", "ulong"},
        {"float", "default(float)", "float"},
        {"double", "default(double)", "double"},
    };
    for (const auto& row : rows) {
        auto r = semaAll4502(prog4502(
            std::string(" var x: ") + row.ty + " := " + row.init + "; " +
            "var v := x[0]; return 0; "));
        BEZEL_EXPECT_TRUE(hasDiag4502(
            r, std::string("type `") + row.spelling +
                   "` does not declare an indexer"));
        BEZEL_EXPECT_TRUE(hasDiag4502(r, kSec234));
    }
}

// ===========================================================================
// T3 — a NULLABLE primitive receiver is rejected too: nullability doesn't
// change whether the primitive KIND declares an indexer.
// ===========================================================================
BEZEL_TEST(sema_subscript_non_indexable_4502,
           t3_nullable_primitive_subscript_rejected_spec_23_4) {
    auto r = semaAll4502(prog4502(
        " var n: int? := 5; var v := n[0]; return 0; "));
    BEZEL_EXPECT_TRUE(hasDiag4502(r, "type `int` does not declare an indexer"));
    BEZEL_EXPECT_TRUE(hasDiag4502(r, kSec234));
}

// ===========================================================================
// T4 — ACCEPT-CONTROLS. Every indexable receiver family stays admitted, each
// RUNS and asserts a value: a false reject here breaks working code, which is
// worse than the bug this rejection fixes.
// ===========================================================================
BEZEL_TEST(sema_subscript_non_indexable_4502,
           t4_string_char_index_still_works_spec_3_4_4_6) {
    auto r = runAotChecked(prog4502(
        " var s: string := \"hello\"; "
        " Console.WriteLine(s[1].ToString()); return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("e") != std::string::npos);
}

BEZEL_TEST(sema_subscript_non_indexable_4502,
           t4_string_range_slice_still_works_spec_3_4_4_6) {
    auto r = runAotChecked(prog4502(
        " var s: string := \"hello\"; "
        " Console.WriteLine(s[1..3]); return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("ell") != std::string::npos);
}

BEZEL_TEST(sema_subscript_non_indexable_4502,
           t4_array_index_still_works_stdlib_4_4) {
    auto r = runAotChecked(prog4502(
        " var arr: Array<int> := [10, 20, 30]; "
        " return arr[1]; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 20);
}

BEZEL_TEST(sema_subscript_non_indexable_4502,
           t4_dictionary_index_still_works_stdlib_4_8) {
    auto r = runAotChecked(prog4502(
        " var d: Dictionary<int, int> := [1: 10, 2: 20]; "
        " return d[2]; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 20);
}

// A user-declared indexer, and the SAME indexer reached through an
// INTERFACE-typed binding — the #423 positive case (#423 fixed the reject
// for an interface receiver with NO indexer; this pins that one WITH an
// indexer still resolves and runs through both the class and interface
// receiver types).
BEZEL_TEST(sema_subscript_non_indexable_4502,
           t4_user_indexer_class_and_interface_receiver_still_work_spec_23_4) {
    const std::string src =
        "module M; "
        "interface IBox4502 "
        "  readonly property [i: int]: int; "
        "end IBox4502; "
        "class Box4502: IBox4502 "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public property [i: int]: int => _v + i; "
        "end Box4502; "
        "class C "
        "  public static method Main(): int "
        "    using var box: Box4502 := new Box4502(10); "
        "    var viaClass := box[5]; "
        "    var ibox: IBox4502 := box; "
        "    var viaInterface := ibox[7]; "
        "    return viaClass + viaInterface; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 32);  // (10+5) + (10+7)
}
