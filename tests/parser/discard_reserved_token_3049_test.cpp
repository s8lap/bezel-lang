// fix #3049 (proposal 0083) — `_` is ALWAYS a discard/wildcard and is NEVER a
// declarable identifier. Spec §1.3 / §17.13 / Appendix A.1 / Appendix B.1, all
// at v0.244 (docs read at v0.247; §1.3, §17.13, A.1 and B.1 are byte-identical
// at both, the graduation having landed at v0.244).
//
// WHAT THE DEFECT WAS. `_` was an ordinary Identifier token, and the discard
// was a set of positional special cases layered on top of it. Everything below
// under "REJECTED" therefore COMPILED CLEAN on the pre-fix tree — verified
// case by case with `build/bezelc -c` before a line of this fix was written:
//
//     var _ := 5;                       rc=0     declared a readable local
//     const _ := 5;                     rc=0
//     var _ := 5; return _;             rc=0     `_` READ — §3.2's "cannot be
//                                                read" was false as written
//     method F(_: int)                  rc=0     `_` in the artifact interface
//     F(ref _)                          rc=0     admitted, unruled (#2679)
//     F(out var _)                      rc=0     declared nothing, silently
//     F(owned _)                        rc=0
//     F(_)                              MALFORMED IR — an `int` formal
//                                                received the scratch slot's
//                                                ADDRESS; `bezelc` failed in
//                                                the LLVM verifier, not sema
//     for var _: int in …               rc=0     ascribed ignore
//     lambda(_: int) => …               rc=0     ascribed ignore
//     lambda(ref _) => …                rc=0     modifier on an ignore
//     case x of var _ =>                rc=0     pattern BINDING named `_`
//     catch var _: Exception =>         rc=0
//     (var _ := 5; 7)                   rc=0     binding-atom clause
//     class _ / type _ / property _ /
//       method _ / module _ / record
//       P(_: int, …) / Box<_>           rc=0     every one of them
//     _: for var i in … do … end for;   rc=0     `_` as a LOOP LABEL
//
// THE FIX is grammatical, not a sema special case, exactly as A.1 now states:
// the Identifier production SPLITS —
//
//     Identifier ::= Letter IdentCont* | "_" IdentCont+
//
// — so the bare one-character `_` matches NEITHER alternative and the lexer
// hands it back as its own reserved token (TokenKind::Discard, B.1 "The
// reserved token `_`"). Every declaration-name slot in the parser is an
// `Identifier` slot, so all of the above stop parsing by construction; each
// gets §17.13's remedy-naming diagnostic instead of a generic "expected …".
// `_f`, `_1`, `__`, `snake_case` are untouched — they satisfy a production.
//
// WHAT IS DELIBERATELY KEPT (§17.13's sanctioned forms — the PART 1 tests):
// the discard statement `_ := e;`, the out-argument discard `out _`, the
// pattern/`when` wildcard, the lambda-parameter and `for`-loop-variable
// IGNOREs, and the anonymous owning binding `using var _ := e;` — including
// its retained ascription `using var _: T := e;`, which the no-annotation rule
// governing the bare ignores explicitly does NOT govern.
//
// The lambda-vs-method-parameter asymmetry (accepted Q2) is load-bearing and
// is pinned in both directions: `lambda(_) => …` is a true ignore, while a
// method/constructor parameter named `_` is rejected because "parameter names
// are call-site-addressable API surface" (§22.5, §2.8.2, A.4 Param).

#include <string>

#include "support/aot_support.hpp"   // runAot — the real bezelc AOT oracle
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// True iff the program is REJECTED and some diagnostic contains `needle`.
// Both halves matter: "any error" would pass on an unrelated failure, and a
// matching string on a program that still compiled would prove nothing.
bool rejects(const std::string& prog, const std::string& needle) {
    auto r = runAot(prog);
    if (!r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// A destructor-bearing class, so ownership is real (a trivial-dispose class
// would take §28.9's ceremony exemption and prove less).
const char* kCell =
    "class Cell public static n: int := 0; public v: int; "
    "  public constructor(x: int) Cell.n := Cell.n + 1; v := x; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; ";

std::string prog(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " end Main; end C;";
}

}  // namespace

// =============================================================================
// PART 1 — the SANCTIONED forms (§17.13). Every one of these must still
// compile AND still mean what it meant; a fix that reserved `_` too broadly
// would break them, which is the over-rejection guard for the whole change.
// =============================================================================

// §17.13 "Discard statement": "`_ := expression;` evaluates `expression` and
// discards its value." For a value type the discard is purely cosmetic.
BEZEL_TEST(discard_reserved_3049, discard_statement_value_type_spec_17_13) {
    BEZEL_EXPECT_EQ(run(prog("_ := 1 + 1; return 7;")), 7);
}

// §17.13 "Non-trivial-dispose class types": the discard is an unbound owned
// temporary "disposed at the end of the discard statement". Cell.n is 1 while
// the statement runs and 0 after it, so reading it AFTER pins the timing.
BEZEL_TEST(discard_reserved_3049, discard_statement_disposes_owned_spec_17_13) {
    BEZEL_EXPECT_EQ(run("module M; " + std::string(kCell) +
                        "class C public static method Main(): int "
                        "  _ := new Cell(4); "
                        "  return Cell.n; "   // 0 — disposed at end of stmt
                        "end Main; end C;"),
                    0);
}

// §17.13 "Discard of a named owned binding": `_ := x;` is an announced
// consume — disposed at end of the discard statement, `x` thereafter treated
// as explicitly disposed.
BEZEL_TEST(discard_reserved_3049, discard_of_named_owned_binding_spec_17_13) {
    BEZEL_EXPECT_EQ(run("module M; " + std::string(kCell) +
                        "class C public static method Main(): int "
                        "  var c := new Cell(4); "
                        "  _ := c; "
                        "  return Cell.n; "
                        "end Main; end C;"),
                    0);
}

// §17.13 "Out-argument position": "`_` discards the out-parameter result."
BEZEL_TEST(discard_reserved_3049, out_argument_discard_spec_17_13) {
    BEZEL_EXPECT_EQ(run("module M; class C "
                        "  public static method Try(out n: int): bool "
                        "    n := 3; return true; "
                        "  end Try; "
                        "  public static method Main(): int "
                        "    if C.Try(out _) then return 9; end; "
                        "    return 0; "
                        "  end Main; end C;"),
                    9);
}

// §17.13 "Pattern position": "`_` matches any value without binding" — both
// the whole-scrutinee catch-all and a destructuring slot (§19.3).
BEZEL_TEST(discard_reserved_3049, wildcard_pattern_spec_17_13) {
    BEZEL_EXPECT_EQ(run(prog("var x := 5; return (case x of 4 => 1, _ => 8);")),
                    8);
}

BEZEL_TEST(discard_reserved_3049, wildcard_destructuring_slot_spec_19_3) {
    BEZEL_EXPECT_EQ(
        run("module M; record P(A: int, B: int); "
            "class C public static method Main(): int "
            "  var p := new P(1, 2); "
            "  return (case p of P(_, _) => 6, _ => 0); "
            "end Main; end C;"),
        6);
}

// v0.289 (§19, §22.4, Appendix A.6/A.7, proposal 0122 L1): the `when`-led
// expression is retired — the replacement is the parenthesized if-expression
// (`(if c then a elif c2 then b else z)`). This test previously verified the
// when expression's result; now verifies that the retired form is rejected.
BEZEL_TEST(discard_reserved_3049, when_catch_all_spec_19_1) {
    BEZEL_EXPECT_TRUE(
        rejects(prog("var x := 1; return (when x > 4 => 2, _ => 5);"),
                "a `when`-led expression is retired"));
}

// §17.13 "Ignore forms" / §27.3: a lambda parameter may be `_`; "because
// nothing is named several ignores may coexist (`lambda(_, _) => 0` is
// legal)". This is the Q2 asymmetry's ACCEPT half — see the method-parameter
// reject below for the other half.
BEZEL_TEST(discard_reserved_3049, lambda_parameter_ignore_spec_27_3) {
    BEZEL_EXPECT_EQ(
        run(prog("var f: (int, int) -> int := lambda(_, _) => 9; "
                 "return f(1, 2);")),
        9);
}

// §27.4: the destructure-slot `_` "is the same nothing-is-bound reading per
// slot" — mixed with a real binding in the same head.
BEZEL_TEST(discard_reserved_3049, lambda_destructure_slot_ignore_spec_27_4) {
    BEZEL_EXPECT_EQ(
        run("module M; record P(A: int, B: int); "
            "class C public static method Main(): int "
            "  var g: (P) -> int := lambda(P(_, bb)) => bb; "
            "  return g(new P(1, 4)); "
            "end Main; end C;"),
        4);
}

// §17.13 / §21.4: "`for var _ in xs do … end;` iterates for effects with the
// element unbound". The body must still run once per element.
BEZEL_TEST(discard_reserved_3049, for_loop_variable_ignore_spec_21_4) {
    BEZEL_EXPECT_EQ(run(prog("var t := 0; "
                             "for var _ in [1, 2, 3] do t := t + 2; end for; "
                             "return t;")),
                    6);
}

// §21.4 / §29.2: "The iteration protocol's borrow/ownership semantics … are
// unchanged by the spelling." Over an OWNING collection (stdlib §4.5) the
// elements stay owned by the collection during a plain iteration and are
// disposed when it is — the ignore changes neither. Encoded as
// iterations*100 + alive-inside*10 + alive-after = 3,3,0. 330 does not fit an
// 8-bit exit status, so the observable goes to stdout via runAotOut (see
// tests/README.md's "Which helper?").
BEZEL_TEST(discard_reserved_3049, for_ignore_owning_iteration_spec_29_2) {
    BEZEL_EXPECT_EQ(
        runAotOut("module M; " + std::string(kCell) +
            "class C public static method Main(): int "
            "  var t := 0; var alive := 0; "
            "  do using var xs := new OwningArray<Cell>(); "
            "     xs.Add(new Cell(1)); xs.Add(new Cell(2)); "
            "     xs.Add(new Cell(3)); "
            "     for var _ in xs do t := t + 1; end for; "
            "     alive := Cell.n; end; "
            "  Console.WriteLine(t * 100 + alive * 10 + Cell.n); "
            "  return 0; "
            "end Main; end C;"),
        std::string("330"));
}

// §17.13 "The anonymous owning binding": `using var _ := e;` is "the one `_`
// form that owns storage" — scope-end disposal, no name bound. Cell.n is still
// 1 inside the scope and 0 after it.
BEZEL_TEST(discard_reserved_3049, anonymous_owning_binding_spec_17_13) {
    BEZEL_EXPECT_EQ(run("module M; " + std::string(kCell) +
                        "class C public static method Main(): int "
                        "  var inside := 0; "
                        "  do using var _ := new Cell(4); inside := Cell.n; end; "
                        "  return inside * 10 + Cell.n; "   // 1 inside, 0 after
                        "end Main; end C;"),
                    10);
}

// §17.13: "The ordinary `using var` ascription is retained (`using var _:
// Stream := …;`): the no-annotation rule governs the bare ignores of this
// section, not this binding." Kept deliberately — pinned so a future tidy-up
// of the ascription rules cannot take it out by accident.
BEZEL_TEST(discard_reserved_3049, anonymous_owning_binding_ascription_retained) {
    BEZEL_EXPECT_EQ(run("module M; " + std::string(kCell) +
                        "class C public static method Main(): int "
                        "  do using var _: Cell := new Cell(4); end; "
                        "  return Cell.n; "
                        "end Main; end C;"),
                    0);
}

// §1.3 / A.1: "Identifiers merely *containing* underscores (`_camelCase`,
// `snake_case`, `_f`) are ordinary identifiers; only the bare single-character
// token is reserved." The `"_" IdentCont+` alternative's boundary, end to end
// through the compiler rather than only in the lexer.
BEZEL_TEST(discard_reserved_3049, underscore_prefixed_identifiers_spec_1_3) {
    BEZEL_EXPECT_EQ(run(prog("var _f := 1; var __ := 2; var _1 := 4; "
                             "var snake_case := 8; "
                             "return _f + __ + _1 + snake_case;")),
                    15);
}

// =============================================================================
// PART 2 — the REJECTED positions (§17.13's own list). Each asserts the
// specific remedy-naming text, not merely "some error": the diagnostic IS the
// deliverable here, and every one of these programs compiled CLEAN pre-fix.
// =============================================================================

// §21.1 / §17.13: "`var _ := e;` is a compile error whose diagnostic names
// both sanctioned spellings — `_ := e;` … or `using var _ := e;`".
BEZEL_TEST(discard_reserved_3049, var_underscore_rejected_two_remedies_21_1) {
    const std::string p = prog("var _ := 5; return 0;");
    BEZEL_EXPECT_TRUE(rejects(p, "cannot be used as a local name"));
    BEZEL_EXPECT_TRUE(rejects(p, "write `_ := e;`"));
    BEZEL_EXPECT_TRUE(rejects(p, "`using var _ := e;`"));
}

// §21.1: "`const _ := e;` is likewise rejected (a never-readable constant is
// useless by construction)".
BEZEL_TEST(discard_reserved_3049, const_underscore_rejected_spec_21_1) {
    BEZEL_EXPECT_TRUE(rejects(prog("const _ := 5; return 0;"),
                              "never-readable constant is useless"));
}

// §17.13 / rationale §3.2: "`_` … never readable as a name". Pre-fix
// `var _ := 5; return _;` compiled and returned 5 — the example that falsified
// the rationale's own line.
BEZEL_TEST(discard_reserved_3049, bare_underscore_not_a_value_spec_17_13) {
    BEZEL_EXPECT_TRUE(rejects(prog("var q := 5; return _;"),
                              "`_` is not a value"));
}

// A.4 Param / §22.5 / §2.8.2 (accepted Q2): a method parameter cannot be named
// `_` — "parameter names are call-site-addressable API surface". The
// diagnostic must ALSO name the lambda asymmetry, since that is the confusion
// the rule invites.
BEZEL_TEST(discard_reserved_3049, method_parameter_underscore_rejected_22_5) {
    const std::string p =
        "module M; class C "
        "  public static method F(_: int): int return 1; end F; "
        "  public static method Main(): int return C.F(1); end Main; end C;";
    BEZEL_EXPECT_TRUE(rejects(p, "cannot be used as a parameter name"));
    BEZEL_EXPECT_TRUE(rejects(p, "call-site-addressable API surface"));
    BEZEL_EXPECT_TRUE(rejects(p, "`lambda(_) => "));
}

BEZEL_TEST(discard_reserved_3049, constructor_parameter_underscore_rejected) {
    BEZEL_EXPECT_TRUE(
        rejects("module M; class C public v: int; "
                "  public constructor(_: int) v := 1; end; "
                "  public static method Main(): int return 0; end Main; end C;",
                "cannot be used as a parameter name"));
}

// §17.13: "**`ref _` is a compile error** — a `ref` argument reads its
// operand, which a discard cannot supply; pass a named location (§22.3)".
// This is the ruling #2679 asked for, arriving via #3049.
BEZEL_TEST(discard_reserved_3049, ref_discard_rejected_spec_17_13) {
    const std::string p =
        "module M; class C "
        "  public static method F(ref n: int): int n := 3; return n; end F; "
        "  public static method Main(): int return C.F(ref _); end Main; end C;";
    BEZEL_EXPECT_TRUE(rejects(p, "`ref _` is a compile error"));
    BEZEL_EXPECT_TRUE(rejects(p, "pass a named location"));
}

// §22.4 / §17.13: "`out var _` is a compile error — it would declare nothing;
// write `out _`, which already means exactly this".
BEZEL_TEST(discard_reserved_3049, out_var_discard_rejected_spec_22_4) {
    const std::string p =
        "module M; class C "
        "  public static method Try(out n: int): bool n := 3; return true; "
        "  end Try; "
        "  public static method Main(): int "
        "    if C.Try(out var _) then return 1; end; return 0; "
        "  end Main; end C;";
    BEZEL_EXPECT_TRUE(rejects(p, "`out var _` is a compile error"));
    BEZEL_EXPECT_TRUE(rejects(p, "it would declare nothing"));
}

// The sibling inline-declaration forms are the same shape — an inline local
// named `_` — and take the same remedy. (§17.13 enumerates `out var _`; the
// `out owned var` / `out using var` spellings are the same "it would declare
// nothing" reading, recorded as this fix's reading of "every declaration named
// `_`".)
BEZEL_TEST(discard_reserved_3049, out_using_var_discard_rejected_spec_22_4) {
    BEZEL_EXPECT_TRUE(
        rejects("module M; " + std::string(kCell) +
                "class C "
                "  public static method Make(out owned c: Cell): bool "
                "    c := new Cell(1); return true; "
                "  end Make; "
                "  public static method Main(): int "
                "    if C.Make(out using var _) then return 1; end; return 0; "
                "  end Main; end C;",
                "is a compile error — it would declare nothing"));
}

// §17.13 / A.4: "in every non-out position — `ref _` included — it is a
// compile error". `owned _` marks a MOVE of a named binding; a discard names
// nothing to move.
BEZEL_TEST(discard_reserved_3049, owned_discard_argument_rejected_spec_22_3) {
    BEZEL_EXPECT_TRUE(
        rejects("module M; " + std::string(kCell) +
                "class C "
                "  public static method Sink(owned c: Cell): int "
                "    c.Dispose(); return 1; "
                "  end Sink; "
                "  public static method Main(): int return C.Sink(owned _); "
                "  end Main; end C;",
                "is a compile error in argument position"));
}

// The unmodified `_` argument — the worst pre-fix outcome in the family: it
// reached CODEGEN and emitted malformed IR (an `int` formal handed the address
// of the compiler's internal scratch slot), so `bezelc` died in the LLVM
// verifier rather than diagnosing anything. Now a clean reject.
BEZEL_TEST(discard_reserved_3049, bare_discard_argument_rejected_spec_22_3) {
    BEZEL_EXPECT_TRUE(
        rejects("module M; class C "
                "  public static method F(n: int): int return n; end F; "
                "  public static method Main(): int return C.F(_); end Main; "
                "end C;",
                "is a compile error in argument position"));
}

// §17.13: "a bare ignore takes its type from context, never an annotation, one
// rule uniform across every ignore form of this section (`out _`,
// lambda-parameter `_`, `for var _`)" — all three arms.
BEZEL_TEST(discard_reserved_3049, out_discard_ascription_rejected_spec_17_13) {
    BEZEL_EXPECT_TRUE(
        rejects("module M; class C "
                "  public static method Try(out n: int): bool n := 3; "
                "    return true; end Try; "
                "  public static method Main(): int "
                "    if C.Try(out _: int) then return 1; end; return 0; "
                "  end Main; end C;",
                "not permitted on the discard `_`"));
}

BEZEL_TEST(discard_reserved_3049, for_ignore_ascription_rejected_spec_21_4) {
    BEZEL_EXPECT_TRUE(
        rejects(prog("for var _: int in [1, 2] do end for; return 0;"),
                "not permitted on the loop-variable ignore"));
}

BEZEL_TEST(discard_reserved_3049, lambda_ignore_ascription_rejected_spec_27_3) {
    BEZEL_EXPECT_TRUE(
        rejects(prog("var f: (int) -> int := lambda(_: int) => 7; "
                     "return f(1);"),
                "not permitted on the bare ignore"));
}

// §27.3: "No parameter modifier … composes with the bare ignore — `FuncParamMods`
// with `_` is rejected (a `ref` parameter reads and writes through its operand,
// which an ignore cannot supply …)".
BEZEL_TEST(discard_reserved_3049, lambda_ignore_modifier_rejected_spec_27_3) {
    BEZEL_EXPECT_TRUE(
        rejects(prog("var v := 1; "
                     "var f: (ref int) -> int := lambda(ref _) => 7; "
                     "return f(ref v);"),
                "no parameter modifier composes with the bare ignore"));
}

// §17.13 / §19.3 (one of the three positions the gate added): "a pattern
// binding (`case x of var _ =>`) … where the bare wildcard already means
// match-without-binding (write `_`)".
BEZEL_TEST(discard_reserved_3049, pattern_binding_underscore_rejected_19_3) {
    const std::string p = prog("var x := 5; return (case x of var _ => 1);");
    BEZEL_EXPECT_TRUE(rejects(p, "cannot be used as a pattern binding name"));
    BEZEL_EXPECT_TRUE(rejects(p, "already matches without binding"));
}

// §17.13 / §31.2: "a catch-arm binding (`catch var _: E =>`) … (write `_`, or
// the bindingless `catch E =>`)".
BEZEL_TEST(discard_reserved_3049, catch_binding_underscore_rejected_31_2) {
    const std::string p =
        prog("try return 0; catch var _: Exception => return 1; end;");
    BEZEL_EXPECT_TRUE(rejects(p, "cannot be used as a catch-arm binding name"));
    BEZEL_EXPECT_TRUE(rejects(p, "`catch E => "));
}

// The `var`-less binding alias the parser also accepts (`catch name: Type`) is
// the same declaration and takes the same rejection.
BEZEL_TEST(discard_reserved_3049, catch_binding_no_var_underscore_rejected) {
    BEZEL_EXPECT_TRUE(
        rejects(prog("try return 0; catch _: Exception => return 1; end;"),
                "cannot be used as a catch-arm binding name"));
}

// §17.13 / §17.15 (the third position the gate added): "a binding-atom clause
// (`(var _ := E; body)`), whose remedy is naming the binding or restructuring
// the expression — the statement remedies above are not spellable inside the
// atom." The diagnostic must NOT offer `_ := e;` / `using var _ := e;` here.
BEZEL_TEST(discard_reserved_3049, binding_atom_clause_underscore_rejected_17_15) {
    const std::string p = prog("return (var _ := 5; 7);");
    BEZEL_EXPECT_TRUE(
        rejects(p, "cannot be used as a binding-atom clause name"));
    BEZEL_EXPECT_TRUE(rejects(p, "name the binding or restructure"));
}

// §17.13 / §21.5: "`_` is not a valid loop label: a label is a declaration,
// and `_` cannot be declared".
BEZEL_TEST(discard_reserved_3049, loop_label_underscore_rejected_spec_21_5) {
    BEZEL_EXPECT_TRUE(
        rejects(prog("_: for var i in [1, 2] do end for; return 0;"),
                "cannot be used as a loop label name"));
}

// §17.13's "every declaration named `_`" tail. The cases below cover a field,
// a property, a method, a type, an alias, a type parameter, a record
// positional field and a module; each was accepted pre-fix.
//
// NOT covered here, though the parser DOES reject them (Parser_Decls.cpp:479
// and :504 for enum constants, :554 and :566 for choice cases, both routed to
// reportDiscardNotDeclarable): enum constant, choice case, import alias, event,
// member const, extern method, case-payload field, and the interface-member
// spellings. The rules are implemented; the tests are missing. Tracked as the
// coverage gap on the issue rather than claimed here — an earlier revision of
// this comment listed "an enum constant" among the covered cases, which was
// not true.
BEZEL_TEST(discard_reserved_3049, field_underscore_rejected_spec_17_13) {
    BEZEL_EXPECT_TRUE(
        rejects("module M; class C private _: int; "
                "  public static method Main(): int return 0; end Main; end C;",
                "cannot be used as a field name"));
}

BEZEL_TEST(discard_reserved_3049, property_underscore_rejected_spec_17_13) {
    BEZEL_EXPECT_TRUE(
        rejects("module M; class C public property _: int => 1; "
                "  public static method Main(): int return 0; end Main; end C;",
                "cannot be used as a property name"));
}

BEZEL_TEST(discard_reserved_3049, method_underscore_rejected_spec_17_13) {
    BEZEL_EXPECT_TRUE(
        rejects("module M; class C public static method _(): int return 2; "
                "  end _; "
                "  public static method Main(): int return 0; end Main; end C;",
                "cannot be used as a method name"));
}

BEZEL_TEST(discard_reserved_3049, type_underscore_rejected_spec_17_13) {
    BEZEL_EXPECT_TRUE(
        rejects("module M; class _ public v: int; end _; "
                "class C public static method Main(): int return 0; "
                "end Main; end C;",
                "cannot be used as a type name"));
}

BEZEL_TEST(discard_reserved_3049, type_alias_underscore_rejected_spec_17_13) {
    BEZEL_EXPECT_TRUE(
        rejects("module M; type _ := int; "
                "class C public static method Main(): int return 0; "
                "end Main; end C;",
                "cannot be used as a type alias name"));
}

BEZEL_TEST(discard_reserved_3049, type_parameter_underscore_rejected_17_13) {
    BEZEL_EXPECT_TRUE(
        rejects("module M; class Box<_> public v: int; end Box; "
                "class C public static method Main(): int return 0; "
                "end Main; end C;",
                "cannot be used as a type parameter name"));
}

BEZEL_TEST(discard_reserved_3049, positional_field_underscore_rejected_17_13) {
    BEZEL_EXPECT_TRUE(
        rejects("module M; record P(_: int, B: int); "
                "class C public static method Main(): int return 0; "
                "end Main; end C;",
                "cannot be used as a positional field name"));
}

BEZEL_TEST(discard_reserved_3049, module_underscore_rejected_spec_17_13) {
    BEZEL_EXPECT_TRUE(
        rejects("module _; class C public static method Main(): int return 0; "
                "end Main; end C;",
                "cannot be used as a module or qualified-name component name"));
}
