// A plain read of a MOVE-ONLY FIELD passed as a call argument that the callee
// consumes — a plain (or `owned`) parameter of a move-only type, or a
// move-only choice-case payload slot — must be rejected at the argument
// (issue #6233; see also #7541, the parameter-position report of the same
// hole; spec §15.3 / §28.8.1 / K9, v0.350).
//
// THE GOVERNING TEXT.
//
//   §15.3 *Parameter passing* (v0.350): "a parameter declared `(x: V)` for an
//   ordinary V is by value (copy); for a move-only V, it is by value-move
//   (transfer ownership). The caller's argument expression is in moved-from
//   state after the call." … "A parameter declared `(owned x: V)` is identical
//   to plain `(x: V)` for a move-only V".
//
//   §15.3 *Whole-value reassignment* (v0.350): "a move-only field is never
//   moved-from (Appendix C.3) — so at a field the plain read `_arr` lends
//   (K1.5): … and `_arr := f(_arr);` is rejected at the argument, a
//   by-value-move parameter requiring the spelled transfer
//   `f(move _arr with fresh)`."
//
//   K9, consequence 1 (v0.350): "every plain read of an owned field lends
//   (K1.5); the only owned read of an owned field or an owning slot is the
//   replacing `move` (§28.8.1)".
//
//   §15.3 *Field placement* (v0.350): a move-only value may be "a case-payload
//   field of a choice", the position K3.12's marker-free store into an owning
//   location, where a borrow is rejected as at any store into an owned
//   location.
//
// THE DEFECT. The `owned` parameter spelling already rejected an `owned`-
// modified field read (§28.8's borrow-into-owned-sink check), but the plain
// spelling — which §15.3 makes identical — accepted it, as did a case-payload
// slot and a field of an owning inline array (which carries no `owned`
// modifier). The callee (or the constructed value) then owned a value the
// field's holder also disposed: a heap-use-after-free (the #6233 case-payload
// repro) or, once the callee disposes its plain parameter (#7482), a double
// free.
//
// THE ORACLE. Each reject row asserts the specific diagnostic and a real
// source location; each accept row runs 100 iterations and checks the exact
// disposal count, so a legal neighbour that started leaking or double-
// disposing fails too.
//
// per §15.3, §28.8.1, §35.4, v0.350

#include <string>

#include "support/aot_support.hpp"  // runAot — the strict AOT oracle
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// `bxExtra` adds members to `Bx`, `progExtra` to `Program`: a reject row's
// offending method must not leak into any other row's program.
std::string decls(const std::string& bxExtra) {
    return std::string(
    "class Res "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res._n := Res._n + 1; end destructor; "
    "end Res; "
    "choice OChoice Text(owned r: Res), Empty; end OChoice; "
    "choice VC A(x: int), B; end VC; "
    "choice Wrap W(inner: OChoice), None; end Wrap; "
    "class Bx "
    "  public owned V: OChoice := OChoice.Empty; "
    "  public owned NV: OChoice? := null; "
    "  public A: [2 owned]Res?; "
    "  public C: VC := VC.B; "
    "  public Cls: Res? := null; "
    "  public constructor() "
    "    V := OChoice.Text(new Res()); NV := OChoice.Text(new Res()); "
    "    A := [new Res(), null]; "
    "  end constructor; "
    "  public method Recv(src: OChoice) using var k := src; end Recv; ") +
    bxExtra + std::string(
    "end Bx; "
    "class Outer "
    "  public owned In: Bx := new Bx(); "
    "  public constructor() end constructor; "
    "end Outer; "
    "class Ct "
    "  public constructor(v: OChoice) using var k := v; end constructor; "
    "end Ct; ");
}

const char* kSinks =
    "public static method Keep(src: OChoice) using var k := src; end Keep; "
    "public static method KeepO(owned src: OChoice) using var k := src; end KeepO; "
    "public static method KeepN(src: OChoice?) using var k := src; end KeepN; "
    "public static method KeepA(src: [2 owned]Res?) end KeepA; "
    "public static method Count(src: OChoice): int using var k := src; return 1; end Count; "
    "public static method Two(n: int, src: OChoice) using var k := src; end Two; "
    "public static method G<T>(x: T) end G; "
    "public static method PeekVC(v: VC) end PeekVC; "
    "public static method PeekR(r: Res?) end PeekR; "
    "public static method Make(): OChoice return OChoice.Text(new Res()); end Make; ";

// A program whose `Run()` body is `body`. Reject rows never run it.
std::string rejectProg(const std::string& body,
                       const std::string& bxExtra = "",
                       const std::string& progExtra = "") {
    return std::string("module M; ") + decls(bxExtra) + " class Program " +
           kSinks + progExtra + " public static method Run() " + body + " end Run; "
           " public static method Main() Run(); end Main; end Program;";
}

// A program running `body` 100 times and printing the disposal count.
std::string loopProg(const std::string& body) {
    return std::string("module M; ") + decls("") + " class Program " + kSinks +
           " public static method Body() " + body + " end Body; "
           " public static method Main() "
           "   var i := 0; "
           "   while i < 100 do Body(); i := i + 1; end while; "
           "   Console.WriteLine(Res._n); "
           " end Main; end Program;";
}

void expectFieldArgRejected(const std::string& body, const char* field,
                            const std::string& bxExtra = "",
                            const std::string& progExtra = "") {
    auto r = runAot(rejectProg(body, bxExtra, progExtra));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    const std::string needle = std::string("cannot pass the field `") + field +
                               "` to a parameter that takes ownership";
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle.c_str()));
}

void expectRunCount(const std::string& body, const std::string& count) {
    auto r = runAot(loopProg(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, count + "\n");
}

}  // namespace

// ---------------------------------------------------------------------------
// Reject rows. The first is #7541's repro: a bare-statement call.
// ---------------------------------------------------------------------------

BEZEL_TEST(move_only_field_read_arg_6233,
           spec_15_3_field_through_local_to_plain_param_bare_statement_rejected) {
    expectFieldArgRejected("var bx := new Bx(); Keep(bx.V);", "V");
}

BEZEL_TEST(move_only_field_read_arg_6233,
           spec_15_3_field_to_plain_param_in_expression_position_rejected) {
    expectFieldArgRejected(
        "var bx := new Bx(); var n := Count(bx.V); Console.WriteLine(n);", "V");
}

BEZEL_TEST(move_only_field_read_arg_6233,
           spec_15_3_field_through_borrowed_param_rejected) {
    expectFieldArgRejected(
        "var bx := new Bx(); ViaParam(bx);", "V", "",
        "public static method ViaParam(b: Bx) Keep(b.V); end ViaParam; ");
}

BEZEL_TEST(move_only_field_read_arg_6233,
           spec_15_3_implicit_this_field_in_non_owned_method_rejected) {
    expectFieldArgRejected(
        "var bx := new Bx(); bx.SelfImplicit();", "V",
        "public method SelfImplicit() Program.Keep(V); end SelfImplicit; ");
}

BEZEL_TEST(move_only_field_read_arg_6233,
           spec_15_3_this_dot_field_in_non_owned_method_rejected) {
    expectFieldArgRejected(
        "var bx := new Bx(); bx.SelfThis();", "V",
        "public method SelfThis() Program.Keep(this.V); end SelfThis; ");
}

BEZEL_TEST(move_only_field_read_arg_6233,
           spec_15_3_null_conditional_field_read_rejected) {
    expectFieldArgRejected("var bx: Bx? := new Bx(); KeepN(bx?.NV);", "NV");
}

BEZEL_TEST(move_only_field_read_arg_6233,
           spec_15_3_force_unwrapped_field_read_rejected) {
    expectFieldArgRejected("var bx := new Bx(); Keep(bx.NV!);", "NV");
}

BEZEL_TEST(move_only_field_read_arg_6233,
           spec_15_3_nested_field_read_rejected) {
    expectFieldArgRejected("var o := new Outer(); Keep(o.In.V);", "V");
}

BEZEL_TEST(move_only_field_read_arg_6233,
           spec_15_3_named_argument_field_read_rejected) {
    expectFieldArgRejected("var bx := new Bx(); Two(src := bx.V, n := 1);", "V");
}

BEZEL_TEST(move_only_field_read_arg_6233,
           spec_15_3_instance_method_argument_field_read_rejected) {
    expectFieldArgRejected(
        "var bx := new Bx(); var b2 := new Bx(); b2.Recv(bx.V);", "V");
}

BEZEL_TEST(move_only_field_read_arg_6233,
           spec_15_3_constructor_argument_field_read_rejected) {
    expectFieldArgRejected("var bx := new Bx(); var c := new Ct(bx.V);", "V");
}

BEZEL_TEST(move_only_field_read_arg_6233,
           spec_15_3_generic_parameter_at_move_only_instantiation_rejected) {
    // §15.3: "A parameter whose declared type is or contains a type parameter
    // is a move-only parameter at every instantiation whose substitution makes
    // that type move-only".
    expectFieldArgRejected("var bx := new Bx(); G(bx.V);", "V");
}

BEZEL_TEST(move_only_field_read_arg_6233,
           spec_15_3_owning_inline_array_field_read_rejected) {
    expectFieldArgRejected("var bx := new Bx(); KeepA(bx.A);", "A");
}

BEZEL_TEST(move_only_field_read_arg_6233,
           spec_15_3_choice_case_payload_field_read_rejected) {
    // #6233's case-constructor repro (measured heap-use-after-free pre-fix).
    expectFieldArgRejected("var bx := new Bx(); var w := Wrap.W(bx.V);", "V");
}

BEZEL_TEST(move_only_field_read_arg_6233,
           spec_15_3_owned_spelling_keeps_its_borrow_into_owned_sink_error) {
    // `(owned x: V)` is identical to `(x: V)` for a move-only V; that spelling
    // already drew §28.8's borrow-into-owned-sink error, which stays.
    auto r = runAot(rejectProg("var bx := new Bx(); KeepO(bx.V);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a borrow cannot be passed where an `owned` value is required"));
}

// ---------------------------------------------------------------------------
// Accept rows: legal neighbours, each disposing exactly once.
// ---------------------------------------------------------------------------

BEZEL_TEST(move_only_field_read_arg_6233,
           spec_15_3_local_call_result_and_case_value_to_plain_param_accepted) {
    // A local (moved), a call result, a case construction, a named local:
    // 4 payloads per iteration, each disposed once by the callee's binding.
    expectRunCount(
        "var a := Make(); Keep(a); "
        "Keep(Make()); "
        "Keep(OChoice.Text(new Res())); "
        "var b := Make(); Two(src := b, n := 1);",
        "400");
}

BEZEL_TEST(move_only_field_read_arg_6233,
           spec_15_3_fresh_value_to_owned_param_and_ctor_accepted) {
    expectRunCount("KeepO(Make()); var c := new Ct(Make());", "200");
}

BEZEL_TEST(move_only_field_read_arg_6233,
           spec_15_3_copyable_and_class_typed_field_reads_accepted) {
    // A value-like choice field (copy), a class-typed field (borrow), an
    // owning-array element (borrow) and a generic call at a copyable
    // instantiation: none is a move-only argument. Each `Bx` disposes its
    // three `Res` (V's payload, NV's payload, A[0]).
    expectRunCount(
        "var bx := new Bx(); PeekVC(bx.C); PeekR(bx.Cls); PeekR(bx.A[0]); "
        "G(bx.C);",
        "300");
}

BEZEL_TEST(move_only_field_read_arg_6233,
           spec_15_3_move_only_field_as_match_scrutinee_accepted) {
    // A borrowing-match scrutinee reads the field in place: no transfer.
    expectRunCount(
        "var bx := new Bx(); "
        "var n := (case bx.V of Text(r) => 1, Empty => 0); "
        "G(n);",
        "300");
}
