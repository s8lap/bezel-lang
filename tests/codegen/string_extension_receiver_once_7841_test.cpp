// Regression tests for #7841 -- a user extension call on a `string`
// receiver that is a CALL RESULT evaluated the receiver expression TWICE
// (`Make.Str(1).Len2()` called `Str` twice): the built-in string arm of
// tryLowerCollectionInstanceCall lowered the receiver, declined the member it
// does not declare, and the extension lowering lowered the receiver again.
//
// Language spec §22.5 (v0.355): an operand is evaluated once, in written
// order. §26.7 (v0.355): the receiver is a borrow of the one value its
// expression denoted when it was evaluated. §26.5 rules 1-2: a name the
// receiver's declared type declares commits to the built-in; any other name
// selects the extension.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

void expectClean(const JitResult& r, const char* out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

const char* kPrelude =
    "module M;\n"
    "public class Make\n"
    "  public static Calls: int := 0;\n"
    "  public static method Str(n: int): string\n"
    "    Make.Calls := Make.Calls + 1;\n"
    "    return \"s\" + n.ToString();\n"
    "  end Str;\n"
    "end Make;\n"
    "public extension string\n"
    "  public method Len2(): int => this.Length;\n"
    "  public method Join(o: string): string => this + o;\n"
    "end;\n"
    "public class Program\n"
    "  public static method Main(): int\n";

std::string program(const std::string& body) {
    return std::string(kPrelude) + body +
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

}  // namespace

BEZEL_TEST(string_extension_receiver_once_7841,
           call_result_receiver_evaluated_once_spec_22_5_26_7) {
    expectClean(
        runAotChecked(program(
            "    var t := 0;\n"
            "    t := t + Make.Str(1).Len2();\n"
            "    Console.WriteLine($\"{t} {Make.Calls}\");\n")),
        "2 1\n");
}

BEZEL_TEST(string_extension_receiver_once_7841,
           call_result_receiver_with_argument_evaluated_once_spec_22_5) {
    expectClean(
        runAotChecked(program(
            "    var s := Make.Str(1).Join(\"x\");\n"
            "    Console.WriteLine($\"{s} {Make.Calls}\");\n")),
        "s1x 1\n");
}

BEZEL_TEST(string_extension_receiver_once_7841,
           owned_string_receiver_loop_no_leak_spec_26_7) {
    expectClean(
        runAotChecked(program(
            "    var t := 0;\n"
            "    for var i in 0..99 do\n"
            "      t := t + Make.Str(i).Len2();\n"
            "    end;\n"
            "    Console.WriteLine($\"{t} {Make.Calls}\");\n")),
        "290 100\n");
}

BEZEL_TEST(string_extension_receiver_once_7841,
           builtin_member_still_answers_declared_name_spec_26_5) {
    expectClean(
        runAotChecked(program(
            "    var n := Make.Str(1).Length;\n"
            "    Console.WriteLine($\"{n} {Make.Calls}\");\n")),
        "2 1\n");
}

BEZEL_TEST(string_extension_receiver_once_7841,
           extension_returning_receiver_no_double_free_spec_26_7) {
    // The result aliases the receiver: the receiver temporary's reference and
    // the result's reference are released independently (ASan: no UAF, no leak).
    expectClean(
        runAotChecked(
            "module M;\n"
            "public class Make\n"
            "  public static method Str(n: int): string\n"
            "    return \"s\" + n.ToString();\n"
            "  end Str;\n"
            "end Make;\n"
            "public extension string\n"
            "  public method Self(): string => this;\n"
            "end;\n"
            "public class Program\n"
            "  public static method Main(): int\n"
            "    var t := 0;\n"
            "    for var i in 0..99 do\n"
            "      var r := Make.Str(i).Self();\n"
            "      t := t + r.Length;\n"
            "    end;\n"
            "    Console.WriteLine(t);\n"
            "    return 0;\n"
            "  end Main;\n"
            "end Program;\n"),
        "290\n");
}

// The fix-forward rows (review of a850ccae, probes p07 / p02 / p06): an
// extension whose result EMBEDS `this` unretained (a record holding the
// receiver string). §26.7 (v0.355): "The receiver is always a borrow". The
// receiver temporary must outlive every read through the result -- an
// immediate post-call release was a heap-use-after-free -- and must still be
// released exactly once (ASan: no UAF, no leak).
namespace {

const char* kWrapPrelude =
    "module M;\n"
    "public record Named(Tag: string, N: int);\n"
    "public extension string\n"
    "  public method Wrap(): Named => new Named(this, 1);\n"
    "end;\n"
    "public class Make\n"
    "  public static method Str(n: int): string => \"s\" + n.ToString();\n"
    "end Make;\n"
    "public class Program\n"
    "  public static method Main(): int\n";

std::string wrapProgram(const std::string& body) {
    return std::string(kWrapPrelude) + body +
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

}  // namespace

BEZEL_TEST(string_extension_receiver_once_7841,
           wrap_result_embeds_receiver_one_shot_no_uaf_spec_26_7) {
    // p07: base printed "a1"; a850ccae crashed in bz_string_to_utf8z.
    expectClean(
        runAotChecked(wrapProgram(
            "    var n := (\"a\" + 1.ToString()).Wrap();\n"
            "    Console.WriteLine(n.Tag);\n")),
        "a1\n");
}

BEZEL_TEST(string_extension_receiver_once_7841,
           wrap_result_bound_to_local_loop_no_uaf_no_leak_spec_26_7) {
    // p02: the record local's own scope-end string-field release owns the
    // receiver (§28.9), exactly as for the static twin `Make.Wrap(Make.Str(i))`.
    expectClean(
        runAotChecked(wrapProgram(
            "    var t := 0;\n"
            "    for var i in 0..199 do\n"
            "      var n := Make.Str(i).Wrap();\n"
            "      t := t + n.Tag.Length;\n"
            "    end;\n"
            "    Console.WriteLine(t);\n")),
        "690\n");
}

BEZEL_TEST(string_extension_receiver_once_7841,
           wrap_result_field_read_inline_loop_no_uaf_no_leak_spec_26_7) {
    // p06: the result is read within the full expression; the receiver is
    // released at its end, after the read (§28.3 K8.1).
    expectClean(
        runAotChecked(wrapProgram(
            "    var t := 0;\n"
            "    for var i in 0..199 do\n"
            "      t := t + Make.Str(i).Wrap().Tag.Length;\n"
            "    end;\n"
            "    Console.WriteLine(t);\n")),
        "690\n");
}

// The third-pass rows (review of 5b3c5d68, probes rr-7841 b02 / c03 / b04 /
// b03a / b03b / a02 / a12 and the issue's own repro). §26.7 (v0.355): "The
// receiver is always a borrow; extensions cannot store `this`, return it as
// `owned`, or dispose it." §28.9 (v0.355): `string` is runtime-managed, and
// that "relaxes nothing about the values the closure captures" -- a closure
// or an async frame that captures the receiver outlives the call legally and
// holds its own reference, as it does for a captured borrowed `string`
// parameter. §30.1 (v0.355): an async method's frame holds its borrows
// until the task settles. A receiver the callee EMBEDS into a value (a
// generic choice, an `Object` box, a nested record, a record handed to a
// parameter) is never released by the caller: leak over use-after-free.
//
// The `runAot` rows keep a pre-existing leak (the embedding shapes, and the
// `Array<() -> int>` element leak c03's static twin shares) and pin the
// absence of the use-after-free: under the ASan sweep
// (`BEZEL_AOT_FORCE_SANITIZE=address`, `detect_leaks=0`) a freed receiver
// read is an exit code other than 0.
namespace {

using bezel::test::runAot;

void expectRan(const JitResult& r, const char* out) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

const char* kStr =
    "public class Make\n"
    "  public static method Str(n: int): string => \"s\" + n.ToString();\n"
    "end Make;\n";

std::string extProgram(const std::string& decls, const std::string& ext,
                       const std::string& body, bool asyncMain = false) {
    return "module M;\n" + decls + kStr + "public extension string\n" + ext +
           "end;\n"
           "public class Program\n" +
           (asyncMain ? "  public static async method Main(): Task<int>\n"
                      : "  public static method Main(): int\n") +
           "    var t := 0;\n" + body +
           "    Console.WriteLine(t);\n"
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

}  // namespace

BEZEL_TEST(string_extension_receiver_once_7841,
           issue_repro_class_field_and_call_result_receivers_spec_22_5) {
    // The issue's own program: base printed "6 2" / "8 4" (each producer ran
    // twice) and leaked the call-result receiver.
    expectClean(
        runAotChecked(
            "module M;\n"
            "public class H\n"
            "  public S: string;\n"
            "  public constructor(n: int) S := \"heap-\" + n.ToString(); end constructor;\n"
            "end H;\n"
            "public class Make\n"
            "  public static Calls: int := 0;\n"
            "  public static method MkH(n: int): owned H Make.Calls := Make.Calls + 1; return new H(n); end MkH;\n"
            "  public static method Str(n: int): string Make.Calls := Make.Calls + 1; return \"s\" + n.ToString(); end Str;\n"
            "end Make;\n"
            "public extension string\n"
            "  public method Len2(): int => this.Length;\n"
            "end;\n"
            "public class Program\n"
            "  public static method Main(): int\n"
            "    var t := 0;\n"
            "    t := t + Make.MkH(1).S.Len2();\n"
            "    Console.WriteLine($\"{t} {Make.Calls}\");\n"
            "    t := t + Make.Str(1).Len2();\n"
            "    Console.WriteLine($\"{t} {Make.Calls}\");\n"
            "    return 0;\n"
            "  end Main;\n"
            "end Program;\n"),
        "6 1\n8 2\n");
}

BEZEL_TEST(string_extension_receiver_once_7841,
           async_extension_frame_pins_receiver_spec_30_1) {
    // b02: the frame resumes after the caller released its temporary.
    expectClean(
        runAotChecked(extProgram(
            "",
            "  public async method LenAsync(): Task<int>\n"
            "    await Task.Delay(1);\n"
            "    return this.Length;\n"
            "  end LenAsync;\n",
            "    for var i in 0..99 do\n"
            "      t := t + await Make.Str(i).LenAsync();\n"
            "    end;\n",
            /*asyncMain=*/true)),
        "290\n");
}

BEZEL_TEST(string_extension_receiver_once_7841,
           returned_lambda_captures_receiver_one_shot_spec_28_9) {
    // b04: the closure outlives the call and its receiver temporary.
    expectClean(
        runAotChecked(extProgram(
            "",
            "  public method Fn(): () -> int => lambda(): int => this.Length;\n",
            "    var f := (\"a\" + 1.ToString()).Fn();\n"
            "    t := f();\n")),
        "2\n");
}

BEZEL_TEST(string_extension_receiver_once_7841,
           void_extension_stores_lambda_over_receiver_no_uaf_spec_28_9) {
    // c03: a VOID member adds a closure over `this` to a caller's array --
    // no return type carries the capture; the capture holds its own +1.
    expectRan(
        runAot(extProgram(
            "",
            "  public method Reg(l: Array<() -> int>) l.Add(lambda(): int => this.Length); end Reg;\n",
            "    using var l := new Array<() -> int>();\n"
            "    for var i in 0..199 do Make.Str(i).Reg(l); end;\n"
            "    for var f in l do t := t + f(); end;\n")),
        "690\n");
}

BEZEL_TEST(string_extension_receiver_once_7841,
           generic_choice_result_embeds_receiver_no_uaf_spec_26_7) {
    // b03a: `Opt2<string>.Some(this)` is not on the alias fact's type list.
    expectRan(
        runAot(extProgram(
            "public choice Opt2<T>\n"
            "  Some(v: T),\n"
            "  Nothing;\n"
            "end Opt2;\n",
            "  public method Som(): Opt2<string> => Opt2<string>.Some(this);\n",
            "    for var i in 0..199 do\n"
            "      var o := Make.Str(i).Som();\n"
            "      t := t + (case o of Some(var v) => v.Length, Nothing => 0);\n"
            "    end;\n")),
        "690\n");
}

BEZEL_TEST(string_extension_receiver_once_7841,
           object_box_result_embeds_receiver_no_uaf_spec_26_7) {
    // b03b: an `Object`-typed result boxes the receiver unretained.
    expectRan(
        runAot(extProgram(
            "",
            "  public method AsObj(): Object => this;\n",
            "    for var i in 0..199 do\n"
            "      var b := Make.Str(i).AsObj();\n"
            "      t := t + b.ToString().Length;\n"
            "    end;\n")),
        "690\n");
}

BEZEL_TEST(string_extension_receiver_once_7841,
           nested_record_result_embeds_receiver_no_uaf_spec_26_7) {
    // a02: the string field sits one record deeper than the alias fact looks.
    expectRan(
        runAot(extProgram(
            "public record Named(Tag: string, N: int);\n"
            "public record Outer(In: Named, K: int);\n",
            "  public method Nest(): Outer => new Outer(new Named(this, 1), 2);\n",
            "    for var i in 0..199 do\n"
            "      var n := Make.Str(i).Nest(); t := t + n.In.Tag.Length + n.K;\n"
            "    end;\n")),
        "1090\n");
}

BEZEL_TEST(string_extension_receiver_once_7841,
           record_result_with_named_argument_no_double_free_spec_28_9) {
    // a12: 5b3c5d68's claim released EVERY string field of the record local
    // at scope end, freeing the caller's `keep` once per iteration.
    expectRan(
        runAot(extProgram(
            "public record P2(A: string, B: string);\n",
            "  public method WrapWith(x: string): P2 => new P2(this, x);\n",
            "    var keep := \"kept\" + 1.ToString();\n"
            "    for var i in 0..199 do\n"
            "      var n := Make.Str(i).WrapWith(keep); t := t + n.A.Length + n.B.Length;\n"
            "    end;\n")),
        "1690\n");
}

BEZEL_TEST(string_extension_receiver_once_7841,
           void_extension_embeds_receiver_in_record_handed_out_no_uaf_spec_26_7) {
    // d01: a VOID member embeds `this` in a record it adds to the caller's
    // array -- the shape no return-type gate can see.
    expectRan(
        runAot(extProgram(
            "public record Named(Tag: string, N: int);\n",
            "  public method Put(l: Array<Named>) l.Add(new Named(this, 1)); end Put;\n",
            "    using var l := new Array<Named>();\n"
            "    for var i in 0..199 do Make.Str(i).Put(l); end;\n"
            "    for var n in l do t := t + n.Tag.Length; end;\n")),
        "690\n");
}

// ---------------------------------------------------------------------------
// Fix-forward rows (the non-author review of d584a839): six programs whose
// `this` reaches a value that outlives the call by a route the first walk
// classified as a READ. Base leaks each; d584a839 released the receiver in
// `main` right after the call and the later read was a heap-use-after-free.
// §26.7 v0.355: "The receiver is always a borrow; extensions cannot store
// `this`, return it as `owned`, or dispose it." The compiler accepts every
// one of these programs, so until a ruling says which of them §26.7 forbids
// the backend keeps base's leak (leak over UAF, the #4322 posture). Each
// row is a `runAot` UAF guard: green on base, red on d584a839 under
// BEZEL_AOT_FORCE_SANITIZE=address.
// ---------------------------------------------------------------------------

namespace {

const char* kHolder =
    "public record Named(Tag: string, N: int);\n"
    "public class Holder\n"
    "  public O: Object := \"\";\n"
    "  public R: Named := new Named(\"\", 0);\n"
    "  private _p: string := \"\";\n"
    "  public property P: string\n"
    "    get => _p;\n"
    "    set => do this.R := new Named(value, 1); end;\n"
    "  end P;\n"
    "end Holder;\n";

}  // namespace

BEZEL_TEST(string_extension_receiver_once_7841,
           bang_operand_embeds_receiver_no_uaf_spec_26_7) {
    // r04: `this!` has the value `this`; the first walk read it as a safe
    // operand whatever the context.
    expectRan(
        runAot(extProgram(
            "public record Named(Tag: string, N: int);\n",
            "  public method W(): Named => new Named(this!, 1);\n",
            "    using var l := new Array<Named>();\n"
            "    for var i in 0..199 do l.Add(Make.Str(i).W()); end;\n"
            "    for var n in l do t := t + n.Tag.Length; end;\n")),
        "690\n");
}

BEZEL_TEST(string_extension_receiver_once_7841,
           as_object_result_boxes_receiver_no_uaf_spec_26_7) {
    // r05: `this as Object` boxes `this` unretained; the first walk treated
    // every type-test operand (`is`, `is not` AND `as`) as a read.
    expectRan(
        runAot(extProgram(
            "",
            "  public method AsO(): Object => this as Object;\n",
            "    using var l := new Array<Object>();\n"
            "    for var i in 0..199 do l.Add(Make.Str(i).AsO()); end;\n"
            "    for var o in l do t := t + o.ToString().Length; end;\n")),
        "690\n");
}

BEZEL_TEST(string_extension_receiver_once_7841,
           object_field_store_boxes_receiver_no_uaf_spec_26_7) {
    // r07: a store to an `Object`-typed class field boxes `this`; only a
    // plain `string` field's store retains.
    expectRan(
        runAot(extProgram(
            kHolder,
            "  public method PutO(h: Holder) h.O := this; end PutO;\n",
            "    using var h := new Holder();\n"
            "    for var i in 0..199 do\n"
            "      Make.Str(i).PutO(h); t := t + h.O.ToString().Length;\n"
            "    end;\n")),
        "690\n");
}

BEZEL_TEST(string_extension_receiver_once_7841,
           property_setter_embeds_receiver_no_uaf_spec_26_7) {
    // r08: the property's user setter embeds `value` in a record (#4322's
    // flow-through); a property store is not a field store.
    expectRan(
        runAot(extProgram(
            kHolder,
            "  public method SetP(h: Holder) h.P := this; end SetP;\n",
            "    using var h := new Holder();\n"
            "    for var i in 0..199 do\n"
            "      Make.Str(i).SetP(h); t := t + h.R.Tag.Length;\n"
            "    end;\n")),
        "690\n");
}

BEZEL_TEST(string_extension_receiver_once_7841,
           out_object_parameter_boxes_receiver_no_uaf_spec_26_7) {
    // r09: `out o: Object` boxes `this` unretained; only `out o: string`
    // retains.
    expectRan(
        runAot(extProgram(
            "",
            "  public method OutO(out o: Object) o := this; end OutO;\n",
            "    for var i in 0..199 do\n"
            "      Make.Str(i).OutO(out var o); t := t + o.ToString().Length;\n"
            "    end;\n")),
        "690\n");
}

BEZEL_TEST(string_extension_receiver_once_7841,
           builtin_tostring_returns_receiver_unretained_no_uaf_spec_26_7) {
    // r18_3: `string.ToString()` returns `this` itself, unretained -- not
    // every built-in `string` member's result is a fresh value.
    expectRan(
        runAot(extProgram(
            "public record Named(Tag: string, N: int);\n",
            "  public method W(): Named => new Named(this.ToString(), 1);\n",
            "    using var l := new Array<Named>();\n"
            "    for var i in 0..199 do l.Add(Make.Str(i).W()); end;\n"
            "    for var n in l do t := t + n.Tag.Length; end;\n")),
        "690\n");
}
