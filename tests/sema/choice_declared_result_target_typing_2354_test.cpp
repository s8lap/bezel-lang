// #2354 -- spec §35.3 *Declared-result target-typing*, v0.340 (with §17.7's
// written-declared-type positions, group (v)).
//
// §35.3 (v0.340): "In a declared-result position — the top-level operand of a
// `return e;` statement in a body with a declared return type; the expression
// body of a member or accessor checked against a declared type; or a `return
// e;` statement or expression body checked against a lambda's return-type
// ascription — where the declared type — for an `async` body, the declared
// `Task<T>`'s result type — nullability stripped, is a choice instantiation
// `C<…>` (or non-generic choice `C`) that declares a case named `N`: the form
// `N(args)` is a case construction of `C<…>.N(args)`, and the bare identifier
// `N` is the payload-free case value `C<…>.N`."
//
// MEASURED on the base (c0c5254f): every bare case name in those positions was
// rejected as `unresolved identifier`, including the spec's own `ParsePercent`
// example and the front-page README's `return UnknownSku(order.Sku);`. The
// accept rows below fail without the fix on that diagnostic; the reject rows
// assert the fix's new text (the arity error, the did-you-mean note), so they
// fail without it too. Rows marked CONTROL pass on both sides: they pin the
// places the rule must NOT reach.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char kFetch[] =
    "public choice Fetch Got(body: string), NotModified, "
    "  Failed(code: int, why: string); end Fetch; "
    "public class Show "
    "  public static method Of(f: Fetch): string "
    "    return (case f of Got(var b) => $\"got {b}\", "
    "      NotModified => \"nm\", Failed(var c, var w) => $\"failed {c} {w}\"); "
    "  end Of; "
    "end Show; ";

bool diagContains(const bezel::test::JitResult& r, const std::string& sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

void expectOut(const std::string& src, const char* want) {
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(want));
}

// A module with kFetch, `members` inside class `Box`, and `main` as Main's body.
std::string fetchProg(const std::string& members, const std::string& main) {
    return std::string("module M; ") + kFetch + "public class Box " + members +
           "end Box; "
           "public class Program "
           "  public static method Main() " +
           main +
           "  end Main; "
           "end Program;";
}

const char kNote[] = "a bare case name constructs only at the top level of a "
                     "`return` operand or expression body";

}  // namespace

// --- accept: the spec's and the README's own programs ------------------------

// §35.3's example, verbatim, driven over all three paths.
BEZEL_TEST(sema_choice_declared_result_2354, spec_35_3_parse_percent_example) {
    expectOut(
        "module M; "
        "public class Program "
        "  public static method ParsePercent(text: string): Result<int, string> "
        "    if not int.TryParse(text, out var value) then "
        "      return Error($\"not a number: {text}\"); "
        "    end; "
        "    if value < 0 or value > 100 then "
        "      return Error($\"out of range: {value}\"); "
        "    end; "
        "    return Ok(value); "
        "  end ParsePercent; "
        "  public static method Main() "
        "    for var t in [\"42\", \"abc\", \"200\"] do "
        "      Console.WriteLine((case ParsePercent(t) of "
        "        Ok(var v) => $\"ok {v}\", Error(var e) => $\"err {e}\")); "
        "    end; "
        "  end Main; "
        "end Program;",
        "ok 42\nerr not a number: abc\nerr out of range: 200\n");
}

// The README shape: an instance method of a class returning a NON-generic
// choice, bare payload cases in `return` under `if`/`elif`.
BEZEL_TEST(sema_choice_declared_result_2354,
           spec_35_3_readme_class_method_nongeneric_choice) {
    expectOut(
        "module M; "
        "public choice Outcome Charged(amount: decimal), "
        "  UnknownSku(sku: string), OutOfStock(left: int); end Outcome; "
        "public class Inventory "
        "  public method Place(sku: string, qty: int, stock: int): Outcome "
        "    if sku = \"ink\" then "
        "      return UnknownSku(sku); "
        "    elif stock < qty then "
        "      return OutOfStock(stock); "
        "    end; "
        "    return Charged(1.25m * qty); "
        "  end Place; "
        "end Inventory; "
        "public class Program "
        "  public static method Main() "
        "    var inv := new Inventory(); "
        "    for var s in [\"pen\", \"ink\", \"pad\"] do "
        "      var pad := s = \"pad\"; "
        "      Console.WriteLine((case inv.Place(s, (if pad then 7 else 10), "
        "          (if pad then 6 else 40)) of "
        "        Charged(var a) => $\"charged {a:F2}\", "
        "        UnknownSku(var k) => $\"no product '{k}'\", "
        "        OutOfStock(var l) => $\"only {l} left\")); "
        "    end; "
        "  end Main; "
        "end Program;",
        "charged 12.50\nno product 'ink'\nonly 6 left\n");
}

// §35.3's second example: an expression body naming a payload-free case.
BEZEL_TEST(sema_choice_declared_result_2354,
           spec_35_3_expression_body_payload_free_case) {
    expectOut(fetchProg("public method CachedOutcome(): Fetch => NotModified; ",
                        "var b := new Box(); "
                        "Console.WriteLine(Show.Of(b.CachedOutcome())); "),
              "nm\n");
}

// §23.2 / §23.3 / §23.4: property expression body, getter arrow body, and both
// indexer spellings are declared-result positions.
BEZEL_TEST(sema_choice_declared_result_2354,
           spec_35_3_property_getter_and_indexer_bodies) {
    expectOut(
        fetchProg("public property Cached: Fetch => NotModified; "
                  "public property Body: Fetch "
                  "  get => Got(\"getter\"); "
                  "end Body; "
                  "public property [i: int]: Fetch "
                  "  get => Failed(i, \"idx\"); "
                  "end; "
                  "public property [a: int, c: int]: Fetch => "
                  "  Failed(a + c, \"idx2\"); ",
                  "var b := new Box(); "
                  "Console.WriteLine(Show.Of(b.Cached)); "
                  "Console.WriteLine(Show.Of(b.Body)); "
                  "Console.WriteLine(Show.Of(b[7])); "
                  "Console.WriteLine(Show.Of(b[1, 2])); "),
        "nm\ngot getter\nfailed 7 idx\nfailed 3 idx2\n");
}

// "A declared nullable `C<…>?` target-types through the stripped form and the
// result converts by §5.1's `T → T?` wrapping."
BEZEL_TEST(sema_choice_declared_result_2354,
           spec_35_3_nullable_declared_result_strips_and_wraps) {
    expectOut(fetchProg("public static method Maybe(n: int): Fetch? "
                        "  if n = 0 then return NotModified; end; "
                        "  if n = 1 then return null; end; "
                        "  return Got(\"maybe\"); "
                        "end Maybe; ",
                        "for var i in [0, 1, 2] do "
                        "  var m := Box.Maybe(i); "
                        "  if m <> null then Console.WriteLine(Show.Of(m)); "
                        "  else Console.WriteLine(\"null\"); end; "
                        "end; "),
              "nm\nnull\ngot maybe\n");
}

// "a written choice instantiation inside a generic body (`method M<T>():
// Result<T, string>`) fires normally" — the declared type supplies the type
// arguments whole, for a payload case and a payload-free one.
BEZEL_TEST(sema_choice_declared_result_2354,
           spec_35_3_generic_choice_type_arguments_from_declared_type) {
    expectOut(
        "module M; "
        "public choice Opt<T> Some(v: T), No; end Opt; "
        "public class Box "
        "  public static method Wrap<T>(x: T): Opt<T> => Some(x); "
        "  public static method Empty<T>(): Opt<T> "
        "    return No; "
        "  end Empty; "
        "  public static method Five(): Opt<int> => Some(5); "
        "end Box; "
        "public class Program "
        "  public static method Main() "
        "    Console.WriteLine((case Box.Wrap<string>(\"w\") of "
        "      Some(var s) => s, No => \"none\")); "
        "    Console.WriteLine((case Box.Empty<int>() of "
        "      Some(var s) => \"some\", No => \"none\")); "
        "    Console.WriteLine((case Box.Five() of "
        "      Some(var s) => $\"{s}\", No => \"none\")); "
        "  end Main; "
        "end Program;",
        "w\nnone\n5\n");
}

// "A redundant parenthesized grouping around the whole operand — `return
// (Ok(v));` — is transparent for this purpose."
BEZEL_TEST(sema_choice_declared_result_2354,
           spec_35_3_parenthesized_whole_operand_is_transparent) {
    expectOut(fetchProg("public static method P(): Fetch "
                        "  return (Got(\"paren\")); "
                        "end P; "
                        "public static method Q(): Fetch => (NotModified); ",
                        "Console.WriteLine(Show.Of(Box.P())); "
                        "Console.WriteLine(Show.Of(Box.Q())); "),
              "got paren\nnm\n");
}

// "for an `async` body, the declared `Task<T>`'s result type".
BEZEL_TEST(sema_choice_declared_result_2354,
           spec_35_3_async_body_reads_task_result_type) {
    expectOut(
        "module M; "
        "public class Box "
        "  public static async method Later(n: int): Task<Result<int, string>> "
        "    if n < 0 then return Error(\"neg\"); end; "
        "    return Ok(n * 2); "
        "  end Later; "
        "end Box; "
        "public class Program "
        "  public static async method Main(): Task<int> "
        "    for var i in [4, -1] do "
        "      var t := await Box.Later(i); "
        "      Console.WriteLine((case t of Ok(var v) => $\"ok {v}\", "
        "        Error(var e) => e)); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;",
        "ok 8\nneg\n");
}

// §27.3: an expression body or `return` checked against a lambda's
// return-type ascription.
BEZEL_TEST(sema_choice_declared_result_2354,
           spec_35_3_ascribed_lambda_body_and_returns) {
    expectOut(
        "module M; "
        "public class Program "
        "  public static method Main() "
        "    var f := lambda(x: int): Result<int, string> => Ok(x + 1); "
        "    var g := lambda(x: int): Result<int, string> => do "
        "      if x > 5 then return Error(\"big\"); end; "
        "      return Ok(x); "
        "    end; "
        "    Console.WriteLine((case f(1) of Ok(var v) => $\"f {v}\", "
        "      Error(var e) => e)); "
        "    Console.WriteLine((case g(9) of Ok(var v) => $\"g {v}\", "
        "      Error(var e) => e)); "
        "  end Main; "
        "end Program;",
        "f 2\nbig\n");
}

// "Payload-argument binding and ownership are exactly the qualified form's":
// an OWNING instantiation (`Ok(owned value: T)` over a class) built bare and
// discharged by `using var`, looped so a per-iteration leak or double free
// shows under the checked oracle. The Ok payload's destructor runs once per
// Ok result.
BEZEL_TEST(sema_choice_declared_result_2354,
           spec_35_3_owning_instantiation_payload_ownership_checked) {
    auto r = runAotChecked(
        "module M; "
        "public class Res "
        "  public static _n: int := 0; "
        "  public id: int; "
        "  public constructor(i: int) id := i; end constructor; "
        "  destructor _n := _n + 1; end destructor; "
        "end Res; "
        "public class Box "
        "  public static method Own(i: int): Result<Res, string> "
        "    if i < 0 then return Error(\"neg\"); end; "
        "    return Ok(new Res(i)); "
        "  end Own; "
        "end Box; "
        "public class Program "
        "  public static method Main(): int "
        "    for var i in 0..99 do "
        "      using var r := Box.Own(i - 50); "
        "    end; "
        "    Console.WriteLine($\"{Res._n}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("50\n"));
}

// "the case reading is authoritative: it wins over any same-named local,
// parameter, member, or type in scope". A same-named static method would have
// produced "member" — the case is constructed instead.
BEZEL_TEST(sema_choice_declared_result_2354,
           spec_35_3_case_reading_wins_over_same_named_member) {
    expectOut(fetchProg("public static method Got(s: string): Fetch "
                        "  => Fetch.Got(\"member\"); "
                        "public static method Use(): Fetch "
                        "  return Got(\"case\"); "
                        "end Use; ",
                        "Console.WriteLine(Show.Of(Box.Use())); "),
              "got case\n");
}

// "... and the compiler warns on such shadowing — naming the case and the
// shadowed declaration, with the remedy". One warning per shadowed kind.
BEZEL_TEST(sema_choice_declared_result_2354,
           spec_35_3_shadowing_warning_names_case_and_declaration) {
    auto r = bezel::test::parseAndTypecheckGated(
        "module M; "
        "public choice Fetch Got(body: string), NotModified; end Fetch; "
        "public class NotModified end NotModified; "
        "public class Box "
        "  public static method Got(s: string): Fetch => Fetch.NotModified; "
        "  public static method A(): Fetch return Got(\"m\"); end A; "
        "  public static method B(NotModified: int): Fetch "
        "    return NotModified; "
        "  end B; "
        "  public static method C(): Fetch "
        "    var NotModified := 3; "
        "    return NotModified; "
        "  end C; "
        "  public static method D(): Fetch => NotModified; "
        "end Box;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    int member = 0, parameter = 0, local = 0, type = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        const std::string& m = d.message;
        if (m.find("in this declared-result position is the case `Fetch.") ==
                std::string::npos ||
            m.find("rename the shadowed binding") == std::string::npos)
            continue;
        if (m.find("shadows the member `Got`") != std::string::npos) ++member;
        if (m.find("shadows the parameter `NotModified`") != std::string::npos)
            ++parameter;
        if (m.find("shadows the local `NotModified`") != std::string::npos)
            ++local;
        if (m.find("shadows the type `NotModified`") != std::string::npos)
            ++type;
    }
    BEZEL_EXPECT_EQ(member, 1);
    BEZEL_EXPECT_EQ(parameter, 1);
    BEZEL_EXPECT_EQ(local, 1);
    BEZEL_EXPECT_EQ(type, 1);
}

// --- reject: arity -------------------------------------------------------------

// "A payload case written bare ... is the arity error §35.5 names": the bare
// form becomes the qualified `Fetch.Got` in value position (#6789's rule).
BEZEL_TEST(sema_choice_declared_result_2354,
           spec_35_3_bare_payload_case_is_the_arity_error) {
    auto r = runAot(fetchProg("public static method P(): Fetch "
                              "  return Got; "
                              "end P; ",
                              ""));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "payload case `Got` cannot be used as a value without its payload"));
}

// "... or a payload-free case written with an argument list".
BEZEL_TEST(sema_choice_declared_result_2354,
           spec_35_3_payload_free_case_with_argument_list_rejected) {
    auto r = runAot(fetchProg("public static method P(): Fetch "
                              "  return NotModified(); "
                              "end P; ",
                              ""));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "case `Fetch.NotModified` has no payload, so it takes no argument "
           "list"));
}

// A wrong payload count is the qualified form's §22.5 binding error (#6119).
BEZEL_TEST(sema_choice_declared_result_2354,
           spec_35_3_payload_count_checked_as_qualified_form) {
    auto r = runAot(fetchProg("public static method P(): Fetch => "
                              "  Got(\"a\", \"b\"); ",
                              ""));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "case `Fetch.Got` supplies 2 argument(s) but the parameter list "
           "has 1"));
}

// --- reject: outside the family (the note names the remedies) ---------------

// "not to subexpression positions within a return operand — in `return (if c
// then Ok(v) else Error(e));` the branches ... resolve ordinarily".
BEZEL_TEST(sema_choice_declared_result_2354,
           spec_35_3_if_expression_branches_do_not_fire) {
    auto r = runAot(fetchProg("public static method P(c: bool): Fetch "
                              "  return (if c then Got(\"a\") else NotModified); "
                              "end P; ",
                              ""));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "unresolved identifier `Got`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "unresolved identifier `NotModified`"));
    BEZEL_EXPECT_TRUE(diagContains(r, kNote));
}

// "not to ... typed local declarations".
BEZEL_TEST(sema_choice_declared_result_2354,
           spec_35_3_typed_local_initializer_does_not_fire) {
    auto r = runAot(fetchProg("public static method P(): Fetch "
                              "  var f: Fetch := Got(\"b\"); "
                              "  return f; "
                              "end P; ",
                              ""));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "unresolved identifier `Got`"));
    BEZEL_EXPECT_TRUE(diagContains(r, kNote));
    BEZEL_EXPECT_TRUE(diagContains(r, "(`Fetch.Got`)"));
}

// "not to argument position".
BEZEL_TEST(sema_choice_declared_result_2354,
           spec_35_3_argument_position_does_not_fire) {
    auto r = runAot(fetchProg("public static method Id(f: Fetch): Fetch => f; "
                              "public static method P(): Fetch "
                              "  return Id(NotModified); "
                              "end P; ",
                              ""));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "unresolved identifier `NotModified`"));
    BEZEL_EXPECT_TRUE(diagContains(r, kNote));
}

// --- CONTROLS: shapes the rule must not reach (pass on both sides) -----------

// "not to lambdas whose return type arrives only by target inference".
BEZEL_TEST(sema_choice_declared_result_2354,
           control_35_3_unascribed_lambda_does_not_fire) {
    auto r = runAot(fetchProg("public static method P(): Fetch "
                              "  var l := lambda() => Got(\"c\"); "
                              "  return Fetch.NotModified; "
                              "end P; ",
                              ""));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "unresolved identifier `Got`"));
}

// "partial qualification does not join the family — `Result.Ok(v)` ...
// remains an error".
BEZEL_TEST(sema_choice_declared_result_2354,
           control_35_3_partial_qualification_still_rejected) {
    auto r = runAot(
        "module M; "
        "public class Program "
        "  public static method P(): Result<int, string> "
        "    return Result.Ok(1); "
        "  end P; "
        "  public static method Main() end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "generic type `Result` requires type "
                                      "arguments"));
}

// "a non-`async` method declared over a `Task`-family type returns a task, not
// a choice, so the case reading never fires in its body"; and "it does not fire
// on a bare type parameter".
BEZEL_TEST(sema_choice_declared_result_2354,
           control_35_3_non_async_task_and_type_parameter_do_not_fire) {
    auto r = runAot(fetchProg("public static method T(): Task<Fetch> "
                              "  return NotModified; "
                              "end T; "
                              "public static method G<R>(): owned R "
                              "  return Got(\"x\"); "
                              "end G; ",
                              ""));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "unresolved identifier `NotModified`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "unresolved identifier `Got`"));
    BEZEL_EXPECT_FALSE(diagContains(r, kNote));
}
