// Regression tests for #7304 (spec §12.3, §5.1, §22.6, v0.355; stdlib §9.1,
// §9.2, §9.4): the `this` / `base` spellings were still admitted as the
// `string` first argument of the `Parse` / `TryParse` family.
//
// ─── The normative sentences ───────────────────────────────────────────────
//
// spec §12.3 (v0.355) — "`base.Name(...)` calls the base class's
// implementation, bypassing virtual dispatch. `base` is legal only inside
// instance members of a derived class." `this` names the ENCLOSING TYPE —
// §9.4 for a value kind's member body ("`this` binds a borrow of the receiver
// value") and §26.1 for an extension block's receiver ("inside an extension
// block, `this` is the receiver").
//
// spec §5.1 (v0.355) — the implicit-conversion table admits NO class-to-
// `string` conversion. The reference conversions below it admit a class to
// `Object` and to a base class; `string` is in neither.
//
// stdlib §9.1 (v0.355) declares the integer tower's parse forms:
//
//   public static pure method Parse(s: string): int;
//   public static method TryParse(s: string, out result: int): bool;
//
// and §9.2 the `double` / `float` forms, §9.4 the `bool` form — the FIRST
// parameter of every one of them is a `string` (or a `ReadOnlyStringSpan`).
//
// spec §22.6 (v0.355) — "If no candidate is applicable at all — no candidate
// is reachable: a conversion §5.1 does not admit as implicit, … the call is a
// compile error with no candidates cited, because no overload was reachable."
//
// ─── The defect ────────────────────────────────────────────────────────────
//
// The resolver deliberately leaves the `this` / `base` spellings UNRESOLVED
// ("Slice 3b: IdentifierExpr name lookup. `this` / `base` are implicit-meaning
// identifiers — leave them unresolved" — src/sema/ExprTypecheck_Exprs.cpp), so
// `ctx_.exprType` of either is null in every ordinary body.
//
// The built-in call admission gate (campaign 2's anchor,
// `ExprTC::checkBuiltinCallAdmission`) asks §5.1's question of every argument
// and reads the argument's STATIC TYPE. With the type missing, the position
// had NO opinion (`builtinArgAdmittedAt` returns "not the admission
// question"), every candidate was applicable, and the call was never refused.
//
// MEASURED on the base arm (the staging tip this lane started from):
//
//     class C
//       public method Go(): int
//         var v: int := 7;
//         var ok: bool := int.TryParse(this, out v);
//         Console.WriteLine(ok);   ->  "False"
//         Console.WriteLine(v);     ->  "0"
//         return 0;
//       end Go;
//     end C;
//
// The receiver's OWN BYTES were read as a `bz.string` descriptor and parsed
// as text — a read of memory the program does not own, and a SILENT one: no
// diagnostic, no abort, and the `out` slot received the failure default 0,
// exactly as it would for a well-formed input that failed to parse.
//
// The class-INSTANCE spelling of the same hole was closed for a LOCAL
// instance by #7290 (and by the anchor's ONE admission gate): a local
// instance is refused with
//
//     no matching overload of `int.TryParse` — … (§22.6)
//
// and the `this` / `base` spellings were the two the local-instance spelling
// left open.
//
// ─── The fix ───────────────────────────────────────────────────────────────
//
// The receiver's type is RECOVERED where the admission gate asks the §5.1
// question — §12.3's own reading of it (the enclosing type; the base class;
// an extension block's head). The refusal is then the compiler's own, at the
// call, with the same diagnostic the local-instance spelling draws. The
// resolver's deferral stands: nothing else reads the recovered type, and
// `ctx_.exprType` is unchanged.
//
// ─── What the rows assert ──────────────────────────────────────────────────
//
// The load-bearing observable is the REFUSAL: the named diagnostic at a real
// location (`failedAtCompileTimeWithRealLocation`), never the absence of a
// diagnostic. The two CONTROL rows pin the other side of the boundary — a
// `string` `this` inside `extension string` (§26.1) and an ordinary local
// `string` still parse, and they are asserted by their RESULT ("True" /
// "255"), so a row cannot pass on a compile that merely did not fail.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// The refusal oracle: the named diagnostic, at a real location, and the
// compiler did not merely abort.
void expectRefused(const JitResult& r, const char* needle) {
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// The program under test: the receiver type's declaration, and a `Main` that
// runs the receiver's method.
std::string prog(const std::string& decls) {
    return "module M;\n" + decls +
           "public class Program\n"
           "  public static method Main(): int\n"
           "    var c := new C();\n"
           "    return c.Go();\n"
           "  end Main;\n"
           "end Program;\n";
}

// A CLASS receiver (§12.3: `this` is the enclosing class).
std::string classDecls(const std::string& body) {
    return "public class C\n"
           "  public method Go(): int\n" + body +
           "    return 0;\n"
           "  end Go;\n"
           "end C;\n";
}

// A DERIVED class (§12.3: `base` is the base class).
std::string derivedDecls(const std::string& body) {
    return "public class B\n"
           "  public F: int := 3;\n"
           "end B;\n"
           "public class C : B\n"
           "  public method Go(): int\n" + body +
           "    return 0;\n"
           "  end Go;\n"
           "end C;\n";
}

// A CLASS with NO base class — where §12.3's "`base` is legal only inside
// instance members of a derived class" makes the `base` spelling itself
// illegal. The compiler says so for the MEMBER-ACCESS form (`base.F`:
// "`C` has no class base") but not for the bare spelling, so the gate still
// has to answer the admission question: a `base` receiver is not a `string`.
std::string classNoBaseDecls(const std::string& body) {
    return "public class C\n"
           "  public method Go(): int\n" + body +
           "    return 0;\n"
           "  end Go;\n"
           "end C;\n";
}

// A RECORD receiver (§9.4: a value kind's member body binds `this` to the
// receiver value).
std::string recordDecls(const std::string& body) {
    return "public record C\n"
           "  public F: int := 3;\n"
           "  public method Go(): int\n" + body +
           "    return 0;\n"
           "  end Go;\n"
           "end C;\n";
}

// The `TryParse(s, out result)` body — stdlib §9.1 / §9.2 / §9.4 declare the
// FIRST parameter as `s: string`.
std::string tryParseBody(const std::string& head, const std::string& slotType,
                          const std::string& slotInit, const std::string& arg) {
    return "    var v: " + slotType + " := " + slotInit + ";\n"
           "    var ok: bool := " + head + ".TryParse(" + arg + ", out v);\n";
}

// The `Parse(s)` body.
std::string parseBody(const std::string& head, const std::string& resultType,
                      const std::string& arg) {
    return "    var n: " + resultType + " := " + head + ".Parse(" + arg + ");\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// THE PIN: `this` in a class is not a `string`.
// ---------------------------------------------------------------------------

// stdlib §9.1 declares `TryParse(s: string, out result: int): bool`. A
// class instance reaches no §9.1 parameter (§5.1 admits no class-to-`string`
// conversion), so §22.6 makes the call a compile error with no candidates
// cited — the SAME diagnostic the local-instance spelling draws (#7290).
BEZEL_TEST(parse_this_base_first_arg_7304,
           class_this_is_not_a_parseable_operand_for_int_tryparse) {
    JitResult r = runAot(prog(classDecls(
        tryParseBody("int", "int", "7", "this"))));
    expectRefused(r, "no matching overload of `int.TryParse`");
}

// The `Parse(s)` form of the same family.
BEZEL_TEST(parse_this_base_first_arg_7304,
           class_this_is_not_a_parseable_operand_for_int_parse) {
    JitResult r = runAot(prog(classDecls(parseBody("int", "int", "this"))));
    expectRefused(r, "no matching overload of `int.Parse`");
}

// The `long` receiver — stdlib §9.1 declares the same shape for it.
BEZEL_TEST(parse_this_base_first_arg_7304,
           class_this_is_not_a_parseable_operand_for_long_tryparse) {
    JitResult r = runAot(prog(classDecls(
        tryParseBody("long", "long", "7", "this"))));
    expectRefused(r, "no matching overload of `long.TryParse`");
}

BEZEL_TEST(parse_this_base_first_arg_7304,
           class_this_is_not_a_parseable_operand_for_long_parse) {
    JitResult r = runAot(prog(classDecls(parseBody("long", "long", "this"))));
    expectRefused(r, "no matching overload of `long.Parse`");
}

// The `double` receiver — stdlib §9.2.
BEZEL_TEST(parse_this_base_first_arg_7304,
           class_this_is_not_a_parseable_operand_for_double_tryparse) {
    JitResult r = runAot(prog(classDecls(
        tryParseBody("double", "double", "0.0", "this"))));
    expectRefused(r, "no matching overload of `double.TryParse`");
}

BEZEL_TEST(parse_this_base_first_arg_7304,
           class_this_is_not_a_parseable_operand_for_double_parse) {
    JitResult r = runAot(prog(classDecls(parseBody("double", "double", "this"))));
    expectRefused(r, "no matching overload of `double.Parse`");
}

// The `bool` receiver — stdlib §9.4. It declares NO span overload, so the
// `ReadOnlyStringSpan` form is not a candidate either.
BEZEL_TEST(parse_this_base_first_arg_7304,
           class_this_is_not_a_parseable_operand_for_bool_tryparse) {
    JitResult r = runAot(prog(classDecls(
        tryParseBody("bool", "bool", "false", "this"))));
    expectRefused(r, "no matching overload of `bool.TryParse`");
}

BEZEL_TEST(parse_this_base_first_arg_7304,
           class_this_is_not_a_parseable_operand_for_bool_parse) {
    JitResult r = runAot(prog(classDecls(parseBody("bool", "bool", "this"))));
    expectRefused(r, "no matching overload of `bool.Parse`");
}

// ---------------------------------------------------------------------------
// THE PIN: `base` in a derived class is not a `string` either (§12.3).
// ---------------------------------------------------------------------------

BEZEL_TEST(parse_this_base_first_arg_7304,
           class_base_is_not_a_parseable_operand_for_int_tryparse) {
    JitResult r = runAot(prog(derivedDecls(
        tryParseBody("int", "int", "7", "base"))));
    expectRefused(r, "no matching overload of `int.TryParse`");
}

BEZEL_TEST(parse_this_base_first_arg_7304,
           class_base_is_not_a_parseable_operand_for_int_parse) {
    JitResult r = runAot(prog(derivedDecls(parseBody("int", "int", "base"))));
    expectRefused(r, "no matching overload of `int.Parse`");
}

BEZEL_TEST(parse_this_base_first_arg_7304,
           class_base_is_not_a_parseable_operand_for_long_tryparse) {
    JitResult r = runAot(prog(derivedDecls(
        tryParseBody("long", "long", "7", "base"))));
    expectRefused(r, "no matching overload of `long.TryParse`");
}

BEZEL_TEST(parse_this_base_first_arg_7304,
           class_base_is_not_a_parseable_operand_for_long_parse) {
    JitResult r = runAot(prog(derivedDecls(parseBody("long", "long", "base"))));
    expectRefused(r, "no matching overload of `long.Parse`");
}

BEZEL_TEST(parse_this_base_first_arg_7304,
           class_base_is_not_a_parseable_operand_for_double_tryparse) {
    JitResult r = runAot(prog(derivedDecls(
        tryParseBody("double", "double", "0.0", "base"))));
    expectRefused(r, "no matching overload of `double.TryParse`");
}

BEZEL_TEST(parse_this_base_first_arg_7304,
           class_base_is_not_a_parseable_operand_for_double_parse) {
    JitResult r = runAot(prog(derivedDecls(parseBody("double", "double", "base"))));
    expectRefused(r, "no matching overload of `double.Parse`");
}

BEZEL_TEST(parse_this_base_first_arg_7304,
           class_base_is_not_a_parseable_operand_for_bool_tryparse) {
    JitResult r = runAot(prog(derivedDecls(
        tryParseBody("bool", "bool", "false", "base"))));
    expectRefused(r, "no matching overload of `bool.TryParse`");
}

BEZEL_TEST(parse_this_base_first_arg_7304,
           class_base_is_not_a_parseable_operand_for_bool_parse) {
    JitResult r = runAot(prog(derivedDecls(parseBody("bool", "bool", "base"))));
    expectRefused(r, "no matching overload of `bool.Parse`");
}

// The `base` spelling in a class with NO base class: §12.3 makes the
// spelling illegal there ("`base` is legal only inside instance members of a
// derived class"), so the call must not be admitted — and the gate's answer
// is the same one: a `base` receiver is not a `string`.
BEZEL_TEST(parse_this_base_first_arg_7304,
           class_base_in_a_class_with_no_base_class_is_not_a_parseable_operand) {
    JitResult r = runAot(prog(classNoBaseDecls(
        tryParseBody("int", "int", "7", "base"))));
    expectRefused(r, "no matching overload of `int.TryParse`");
}

// ---------------------------------------------------------------------------
// THE PIN: a RECORD's `this` is the receiver VALUE (§9.4), not a `string`.
// ---------------------------------------------------------------------------

BEZEL_TEST(parse_this_base_first_arg_7304,
           record_this_is_not_a_parseable_operand_for_int_tryparse) {
    JitResult r = runAot(prog(recordDecls(
        tryParseBody("int", "int", "7", "this"))));
    expectRefused(r, "no matching overload of `int.TryParse`");
}

BEZEL_TEST(parse_this_base_first_arg_7304,
           record_this_is_not_a_parseable_operand_for_int_parse) {
    JitResult r = runAot(prog(recordDecls(parseBody("int", "int", "this"))));
    expectRefused(r, "no matching overload of `int.Parse`");
}

BEZEL_TEST(parse_this_base_first_arg_7304,
           record_this_is_not_a_parseable_operand_for_double_tryparse) {
    JitResult r = runAot(prog(recordDecls(
        tryParseBody("double", "double", "0.0", "this"))));
    expectRefused(r, "no matching overload of `double.TryParse`");
}

BEZEL_TEST(parse_this_base_first_arg_7304,
           record_this_is_not_a_parseable_operand_for_bool_tryparse) {
    JitResult r = runAot(prog(recordDecls(
        tryParseBody("bool", "bool", "false", "this"))));
    expectRefused(r, "no matching overload of `bool.TryParse`");
}

// ---------------------------------------------------------------------------
// THE CONTROLS: the recovery must not refuse a receiver that IS a `string`.
// ---------------------------------------------------------------------------

// §26.1: inside an extension block, `this` IS the receiver — and for
// `extension string` that receiver is a `string`. The call is legal, and it
// PARSES: the row is asserted by its result, not by the absence of a
// diagnostic.
BEZEL_TEST(parse_this_base_first_arg_7304,
           a_string_this_inside_extension_string_still_parses) {
    JitResult r = runAot(
        "module M;\n"
        "extension string\n"
        "  public method TryParseInt(out result: int): bool => "
        "int.TryParse(this, out result);\n"
        "end;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var s := \"255\";\n"
        "    var v: int := 7;\n"
        "    var ok: bool := s.TryParseInt(out v);\n"
        "    Console.WriteLine(ok);\n"
        "    Console.WriteLine(v);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n255\n"));
}

// The same call with an ordinary local `string` — the admitting twin of every
// refusal row above (a legal value at the same position). It parses.
BEZEL_TEST(parse_this_base_first_arg_7304,
           a_local_string_still_parses) {
    JitResult r = runAot(
        "module M;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var s := \"255\";\n"
        "    var v: int := 7;\n"
        "    var ok: bool := int.TryParse(s, out v);\n"
        "    Console.WriteLine(ok);\n"
        "    Console.WriteLine(v);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n255\n"));
}

// The LOCAL-INSTANCE twin of the `this` rows: #7290's refusal stands, with
// the same diagnostic — the recovery did not change it.
BEZEL_TEST(parse_this_base_first_arg_7304,
           a_local_class_instance_is_still_refused) {
    JitResult r = runAot(
        "module M;\n"
        "public class D\n"
        "  public F: int := 3;\n"
        "end D;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var o := new D();\n"
        "    var v: int := 7;\n"
        "    var ok: bool := int.TryParse(o, out v);\n"
        "    Console.WriteLine(ok);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    expectRefused(r, "no matching overload of `int.TryParse`");
}
