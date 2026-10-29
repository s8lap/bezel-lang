// Regression tests for #7775 -- a user `extension Object` member called on a
// built-in `StringBuilder` receiver was silently never called: sema's
// extension matcher treated `Object` as the base of a receiver only when the
// receiver had a user declaration, so the member access stayed untyped and
// codegen dropped the call (`new StringBuilder("a").Append("b").KeepO(h)
// .Length` exited 70 with a NullReferenceException because `h.o` was never
// stored; binding the result was rejected with "`Tag` on built-in type
// `StringBuilder` produced no value").
//
// Language spec §26.5 rule 2 (v0.350): the compiler searches "for extension
// blocks on `x`'s type, base types, or implemented interfaces"; §8.1: every
// type derives from `Object`. So an `extension Object` block applies to a
// `StringBuilder` and to a built-in collection.
//
// Rule 1 (v0.350): "If `x`'s declared type (or any base type) *declares* any
// instance member named `Foo` -- of any arity, applicability, or
// accessibility -- instance resolution is committed and extension blocks are
// **never consulted** for that name on that receiver." stdlib §3.1 declares
// `Length`, `Capacity`, `Append`, `AppendLine`, `AppendFormat`, `Insert`,
// `Remove`, `Replace`, `Clear` and `ToString`; `Object` (§8.1) declares
// `ToString`, `Equals`, `GetHashCode` and `GetType`. So a same-named
// `Object` member never shadows one of those (the first attempt at this fix
// let `extension Object { Append(..): int }` answer `sb.Append("c")`).
//
// §26.5.1 (v0.350): "An extension on E1 is more specific than one on E2 when
// E1 is a subtype of E2 and E2 is not a subtype of E1"; `Object` is a
// supertype of every type, so `extension Object` is the least specific
// candidate and never ties an `IEnumerable<T>` / `Array<T>` block (the first
// attempt reported such a pair as ambiguous, and shadowed the stdlib §5
// operators with a same-named `Object` member).
//
// Rows whose receiver is an unbound fresh `new StringBuilder(...)` run on
// runAotCheckedLenient: the temporary is not claimed for disposal when the
// call's terminal is a user extension (#7565's gate), so it keeps that
// pre-existing leak; generated-code ASan stays on. Every other row runs on
// runAotChecked.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

void expectClean(const JitResult& r, const char* out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

void expectRejected(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, needle));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

std::string mainProgram(const std::string& decls, const std::string& body) {
    return "module M;\n" + decls +
           "class C\n"
           "  public static method Tag(s: string): string\n"
           "    Console.WriteLine(\"eval \" + s);\n"
           "    return s;\n"
           "  end Tag;\n"
           "  public static method Main(): int\n" +
           body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

const char* kStoreExt =
    "class H\n"
    "  public o: Object?;\n"
    "  public constructor() end constructor;\n"
    "  public method Keep(x: Object) this.o := x; end Keep;\n"
    "end H;\n"
    "extension Object\n"
    "  public method KeepO(h: H): string\n"
    "    h.Keep(this);\n"
    "    return \"k\";\n"
    "  end KeepO;\n"
    "  public method KeepV(h: H)\n"
    "    h.Keep(this);\n"
    "  end KeepV;\n"
    "end;\n";

}  // namespace

// ── the DEFECT: the `extension Object` member was never called ─────────────

BEZEL_TEST(object_extension_builtin_receiver_7775,
           issue_f01_chained_receiver_length_loop_100_spec_26_5_rule_2) {
    expectClean(
        runAotCheckedLenient(mainProgram(
            kStoreExt,
            "    var h := new H();\n"
            "    var tot := 0;\n"
            "    for var i in 0..<100 do\n"
            "      tot := tot + new StringBuilder(\"a\").Append(\"b\").KeepO(h).Length;\n"
            "    end;\n"
            "    Console.WriteLine(h.o!.ToString());\n"
            "    Console.WriteLine(tot);\n")),
        "ab\n100\n");
}

BEZEL_TEST(object_extension_builtin_receiver_7775,
           issue_f02_whole_statement_loop_100_spec_26_5_rule_2) {
    expectClean(
        runAotCheckedLenient(mainProgram(
            kStoreExt,
            "    var h := new H();\n"
            "    for var i in 0..<100 do\n"
            "      new StringBuilder(\"a\").Append(\"b\").KeepV(h);\n"
            "    end;\n"
            "    Console.WriteLine(h.o!.ToString());\n")),
        "ab\n");
}

BEZEL_TEST(object_extension_builtin_receiver_7775,
           issue_f01b_bound_result_on_bound_builder_spec_26_5_rule_2) {
    // Binding the result used to be rejected with "produced no value". The
    // user-class receiver is the control.
    expectClean(
        runAotChecked(mainProgram(
            "extension Object\n"
            "  public method Tag(): string\n"
            "    Console.WriteLine(\"tag called\");\n"
            "    return \"k\";\n"
            "  end Tag;\n"
            "end;\n"
            "class P\n"
            "  public constructor() end constructor;\n"
            "end P;\n",
            "    var b := new StringBuilder(\"a\");\n"
            "    var s := b.Tag();\n"
            "    Console.WriteLine(s);\n"
            "    var p := new P();\n"
            "    Console.WriteLine(p.Tag());\n")),
        "tag called\nk\ntag called\nk\n");
}

BEZEL_TEST(object_extension_builtin_receiver_7775,
           receiver_chain_evaluated_once_spec_26_5_rule_2_spec_26_7) {
    // §26.7: the receiver is a borrow of the ONE value the receiver
    // expression produced.
    expectClean(
        runAotCheckedLenient(mainProgram(
            "extension Object\n"
            "  public method ObjTag(): int => 5;\n"
            "end;\n",
            "    var n := new StringBuilder(C.Tag(\"a\")).Append(C.Tag(\"b\")).ObjTag();\n"
            "    Console.WriteLine(n);\n")),
        "eval a\neval b\n5\n");
}

BEZEL_TEST(object_extension_builtin_receiver_7775,
           this_is_the_builder_inside_the_extension_spec_26_5_rule_2) {
    expectClean(
        runAotChecked(mainProgram(
            "extension Object\n"
            "  public method Show(): string => \"<\" + this.ToString() + \">\";\n"
            "end;\n",
            "    var b := new StringBuilder(\"abc\");\n"
            "    b.Append(\"d\");\n"
            "    Console.WriteLine(b.Show());\n")),
        "<abcd>\n");
}

BEZEL_TEST(object_extension_builtin_receiver_7775,
           null_conditional_call_on_nullable_builder_spec_26_5_rule_2_spec_7_5) {
    // The `?.` twin: before the fix `m?.Two()` lowered to nothing.
    expectClean(
        runAotChecked(mainProgram(
            "extension Object\n"
            "  public method Two(): int => 2;\n"
            "end;\n",
            "    var keep := new StringBuilder(\"k\");\n"
            "    var tot := 0;\n"
            "    for var i in 0..<100 do\n"
            "      var m: StringBuilder? := keep;\n"
            "      if i mod 2 = 1 then m := null; end;\n"
            "      tot := tot + (m?.Two() ?? 1000);\n"
            "    end;\n"
            "    Console.WriteLine(tot);\n")),
        "50100\n");
}

BEZEL_TEST(object_extension_builtin_receiver_7775,
           array_stack_queue_receivers_reach_object_extension_spec_26_5_rule_2) {
    expectClean(
        runAotChecked(mainProgram(
            "extension Object\n"
            "  public method Who(): string => \"object\";\n"
            "end;\n",
            "    var a := new Array<string>();\n"
            "    var s := new Stack<int>();\n"
            "    var q := new Queue<int>();\n"
            "    Console.WriteLine(a.Who() + s.Who() + q.Who());\n")),
        "objectobjectobject\n");
}

// ── §26.5.1: `Object` is the LEAST specific candidate ──────────────────────

BEZEL_TEST(object_extension_builtin_receiver_7775,
           more_specific_builtin_blocks_beat_object_spec_26_5_1) {
    expectClean(
        runAotChecked(mainProgram(
            "extension Object\n"
            "  public method Who(): string => \"object\";\n"
            "end;\n"
            "extension StringBuilder\n"
            "  public method Who(): string => \"sb\";\n"
            "end;\n"
            "extension IEnumerable<int>\n"
            "  public method Who(): string => \"ienum\";\n"
            "end;\n"
            "extension Array<T>\n"
            "  public method Who(): string => \"array\";\n"
            "end;\n",
            "    var b := new StringBuilder(\"x\");\n"
            "    var a := new Array<int>();\n"
            "    var s := new Stack<int>();\n"
            "    var q := new Array<string>();\n"
            "    Console.WriteLine(b.Who() + \" \" + a.Who() + \" \" + s.Who() + \" \" + q.Who());\n")),
        "sb array ienum array\n");
}

BEZEL_TEST(object_extension_builtin_receiver_7775,
           ienumerable_block_beats_object_not_ambiguous_spec_26_5_1) {
    // The first attempt reported this pair as "equally specific".
    expectClean(
        runAotChecked(mainProgram(
            "extension Object\n"
            "  public method Hello(): string => \"obj\";\n"
            "end;\n"
            "extension IEnumerable<T>\n"
            "  public method Hello(): string => \"ienum\";\n"
            "end;\n",
            "    var a := new Array<int>();\n"
            "    Console.WriteLine(a.Hello());\n")),
        "ienum\n");
}

BEZEL_TEST(object_extension_builtin_receiver_7775,
           stdlib_linq_operator_beats_same_named_object_member_spec_26_5_1) {
    // stdlib §5's `First` / `Sum` / `Max` are extensions over IEnumerable<T>,
    // more specific than `Object`: the operator answers, not the user member.
    expectClean(
        runAotChecked(mainProgram(
            "extension Object\n"
            "  public method First(): int => 4241;\n"
            "  public method Sum(): int => 4242;\n"
            "  public method Max(): int => 4243;\n"
            "end;\n",
            "    var a := new Array<int>();\n"
            "    a.Add(5); a.Add(6);\n"
            "    Console.WriteLine(a.First());\n"
            "    Console.WriteLine(a.Sum());\n"
            "    Console.WriteLine(a.Max());\n")),
        "5\n11\n6\n");
}

// ── rule 1: a DECLARED name is never answered by an `Object` member ────────

BEZEL_TEST(object_extension_builtin_receiver_7775,
           declared_builder_members_win_over_object_members_spec_26_5_rule_1) {
    expectClean(
        runAotChecked(mainProgram(
            "extension Object\n"
            "  public method Append(x: string): int => 4242;\n"
            "  public method Clear(): string => \"EXTCLEAR\";\n"
            "  public method Insert(i: int, s: string): int => 7;\n"
            "  public method Replace(a: string, b: string): int => 8;\n"
            "end;\n",
            "    var sb := new StringBuilder(\"ab\");\n"
            "    var r := sb.Append(\"c\");\n"
            "    Console.WriteLine(r.ToString());\n"
            "    sb.Append(\"d\").Append(\"e\");\n"
            "    Console.WriteLine(sb.Length);\n"
            "    var c := sb.Clear();\n"
            "    Console.WriteLine(c.Length);\n"
            "    var q := sb.Insert(0, \"zz\");\n"
            "    var w := q.Replace(\"z\", \"y\");\n"
            "    Console.WriteLine(w.ToString());\n")),
        "abc\n5\n0\nyy\n");
}

BEZEL_TEST(object_extension_builtin_receiver_7775,
           declared_collection_members_win_over_object_members_spec_26_5_rule_1) {
    expectClean(
        runAotChecked(mainProgram(
            "extension Object\n"
            "  public method Add(x: int): string => \"EXT\";\n"
            "  public method Contains(x: int): int => 55;\n"
            "end;\n",
            "    var a := new Array<int>();\n"
            "    a.Add(5);\n"
            "    Console.WriteLine(a.Count);\n"
            "    Console.WriteLine(a.Contains(5));\n")),
        "1\nTrue\n");
}

BEZEL_TEST(object_extension_builtin_receiver_7775,
           object_base_members_are_not_shadowed_on_an_array_spec_26_5_rule_1_spec_8_1) {
    // `Object` declares `ToString` (§8.1), so `a.ToString(3)` commits to it
    // and fails -- it never reaches a same-named `Object` extension member.
    expectRejected(
        mainProgram(
            "extension Object\n"
            "  public method ToString(n: int): string => \"EXT\";\n"
            "end;\n",
            "    var a := new Array<int>();\n"
            "    Console.WriteLine(a.ToString(3));\n"),
        "the universal `Object.ToString` member takes 0 argument(s)");
}

BEZEL_TEST(object_extension_builtin_receiver_7775,
           builder_length_property_called_with_an_argument_is_rejected_spec_26_5_rule_1) {
    // `Length` is a §3.1 property: the committed lookup fails, and neither
    // the built-in's value nor the same-named extension answers.
    const char* kNeedle =
        "`Length` on `StringBuilder` is an instance member of the receiver's "
        "declared type";
    expectRejected(
        mainProgram(
            "extension Object\n"
            "  public method Length(k: int): int => 99 + k;\n"
            "end;\n",
            "    var sb := new StringBuilder(\"abc\");\n"
            "    Console.WriteLine(sb.Length(1));\n"),
        kNeedle);
    expectRejected(
        mainProgram("",
                    "    var sb := new StringBuilder(\"abc\");\n"
                    "    Console.WriteLine(sb.Length(1));\n"),
        kNeedle);
}
