// Codegen — issue #2972. `this.Message` (and `base.Message`) inside a user
// exception subclass's own body reads the inherited §31.4 slot, exactly as the
// unqualified `Message` spelling already did.
//
// Spec: §31.4 "Standard exception types" declares the base surface verbatim —
//
//     public class Exception
//       public pure constructor();
//       public pure constructor(message: string);
//       public constructor(message: string, cause: Exception);
//       public readonly property Message: string;
//       public readonly property StackTrace: string;
//       public readonly property CauseChain: Array<ExceptionInfo>;
//     end Exception;
//
// — §12.1 makes a base class's members available on a derived class, §12.3
// makes `base.Name` the way to reach a base implementation, §11 puts no
// restriction on the `this.`-qualified spelling, and §21.7 requires a method
// with a return type to `return expression;` on every exit path. v0.240.
//
// THE DEFECT. Every codegen path that recognises a §31.4 header read keys on
// the RECEIVER's static type, `ctx_.exprType(*ma->target)`. Sema deliberately
// gives the `this` / `base` identifier NO type inside a named-type member body
// — `ExprTC::resolveMemberAccess` reads `currentType_` instead and says so in
// its own comment — so that type is null and the whole handler was skipped.
// The access then fell through every remaining arm of `lowerExpr` and the
// caller substituted a degrade.
//
// ONE missing receiver type, FOUR outcomes, decided purely by which frame the
// read is written in. MEASURED on 618c0c8, all on one `class ExT: Exception`
// constructed with "zz"/"abcd"/"ee":
//
//   method body        `return this.Message;`
//                      -> "internal: return-value expression failed to lower to
//                         a value (degraded to a zero/null return value) —
//                         #2491", then a NULL string returned; the caller
//                         SIGSEGV'd on it (exit 139; under ASan "SEGV on unknown
//                         address 0x0000000000008 (READ) ... Hint: address
//                         points to the zero page").
//   constructor body   `_n := this.Message.Length;`
//                      -> NO diagnostic at all; stored 0 where the message is
//                         four characters long.
//   destructor body    `Console.WriteLine("dtor=" + this.Message);`
//                      -> NO diagnostic at all; the whole statement vanished.
//   property / lambda  -> NO diagnostic at all; the accessor's enclosing
//                         statement vanished / the lambda returned 0.
//
// Two of the four are silent, so every case below asserts the program's OUTPUT.
// "It compiled" and "exit 0" both report the silent ones as success.
//
// The UNQUALIFIED spelling (`Message`, #625) worked in all four frames: its own
// bare-identifier arm keys on `currentThis_`, never on a receiver type. The
// comment on sema's bare-`Message` resolver asserted the two spellings "share a
// lowering ... (same as `this.Message` — see the MemberAccessExpr case in
// LlvmBackend_Exprs.cpp CG22+)"; this test is the refutation of that claim and
// its repair.
//
// THE FIX'S GATE, and why it is not a frame list. The byte-8 read is correct
// exactly when the object carries the §31.4 header, which `classStruct` inserts
// for exactly the classes `sema::builtinExceptionBaseOf` accepts — the ONE walk
// #2950 established. Sema's own arm for this access asks that SAME function, of
// the lexically enclosing type, which IS the type of `this`; so "sema resolved
// this access as the inherited Message" is that property, already decided by
// the same predicate, and the two cannot diverge. It is observable in codegen
// as: the access is typed `string` and carries no member symbol. A subclass
// that declares its OWN `Message` resolves to that member's symbol and is
// excluded — pinned below. Keying on sema's resolution rather than re-deriving
// the enclosing type in codegen is what makes this total over frames: an
// earlier draft recovered the enclosing type by reverse-looking `currentFn_` in
// the emitters' memo caches (as `currentReceiverRecordType` / `choiceTypeOf`
// do) and left the property and lambda frames still broken, because an inlined
// accessor body re-points `currentThis_` while `currentFn_` still belongs to
// the enclosing method.
//
// Mechanical verification: with `src/` reverted to 618c0c8 and these tests
// kept, 12 of the 16 cases below FAIL and 4 pass — the 4 being the bare-
// spelling control, the from-outside control, and the two own-`Message`
// negatives, i.e. exactly the cases that do not read the inherited slot
// through `this` / `base`.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

bool prints(const std::string& program, const std::string& expected) {
    return runAotOut(program) == expected;
}

// A subclass of `Exception` with a (message) constructor, plus `body` members,
// exercised by `main`.
std::string prog(const std::string& members, const std::string& main) {
    return "module M; "
           "class ExT: Exception "
           "  public constructor(m: string) base(m); end constructor; " +
           members +
           "end ExT; "
           "class C public static method Main(): int " +
           main +
           "  return 0; "
           "end Main; end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// #2972's own repro: a method body. The `internal:` note plus a null string
// plus a SIGSEGV at the first use.
// ---------------------------------------------------------------------------

BEZEL_TEST(codegen_this_exception_message_2972, qualified_message_in_method_body_spec_31_4) {
    BEZEL_EXPECT_TRUE(prints(
        prog("  public method Qualified(): string "
             "    return this.Message; "
             "  end Qualified; ",
             "  using var l: ExT := new ExT(\"zz\"); "
             "  Console.WriteLine(\"qual=\" + l.Qualified()); "),
        "qual=zz"));
}

// The unqualified form, in the same class and the same slot — the control that
// makes the qualified case's failure specific to the spelling (#625).
BEZEL_TEST(codegen_this_exception_message_2972, bare_message_in_method_body_control_625) {
    BEZEL_EXPECT_TRUE(prints(
        prog("  public method Bare(): string "
             "    return Message; "
             "  end Bare; ",
             "  using var l: ExT := new ExT(\"zz\"); "
             "  Console.WriteLine(\"bare=\" + l.Bare()); "),
        "bare=zz"));
}

// Both spellings in ONE class, so a fix that repairs the qualified form by
// breaking the bare one cannot pass.
BEZEL_TEST(codegen_this_exception_message_2972, both_spellings_agree_spec_31_4) {
    BEZEL_EXPECT_TRUE(prints(
        prog("  public method Bare(): string return Message; end Bare; "
             "  public method Qualified(): string return this.Message; "
             "  end Qualified; ",
             "  using var l: ExT := new ExT(\"zz\"); "
             "  Console.WriteLine(\"bare=\" + l.Bare()); "
             "  Console.WriteLine(\"qual=\" + l.Qualified()); "),
        "bare=zz\nqual=zz"));
}

// A chained read (`this.Message.Length`) — the access is a receiver here, not
// the whole return expression, so it exercises the same handler in value
// position rather than through §21.7's return lowering.
BEZEL_TEST(codegen_this_exception_message_2972, qualified_message_chained_length_spec_31_4) {
    BEZEL_EXPECT_TRUE(prints(
        prog("  public method QualLen(): int "
             "    return this.Message.Length; "
             "  end QualLen; ",
             "  using var l: ExT := new ExT(\"zz\"); "
             "  Console.WriteLine(\"len=\" + l.QualLen().ToString()); "),
        "len=2"));
}

// ---------------------------------------------------------------------------
// The frame axis. Three more frames, two of which failed SILENTLY.
// ---------------------------------------------------------------------------

// §22.7: a constructor body, after the `base(m)` chain has stored the slot.
// Pre-fix this compiled with no diagnostic and stored 0.
BEZEL_TEST(codegen_this_exception_message_2972, qualified_message_in_constructor_body_spec_22_7) {
    BEZEL_EXPECT_TRUE(prints(
        "module M; "
        "class ExC: Exception "
        "  private _n: int; "
        "  public constructor(m: string) "
        "    base(m); "
        "    _n := this.Message.Length; "
        "  end constructor; "
        "  public method N(): int return _n; end N; "
        "end ExC; "
        "class C public static method Main(): int "
        "  using var c := new ExC(\"abcd\"); "
        "  Console.WriteLine(\"n=\" + c.N().ToString()); "
        "  return 0; "
        "end Main; end C;", "n=4"));
}

// §28.4: a destructor body. Pre-fix the whole statement vanished, silently.
BEZEL_TEST(codegen_this_exception_message_2972, qualified_message_in_destructor_body_spec_28_4) {
    BEZEL_EXPECT_TRUE(prints(
        "module M; "
        "class ExD: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "  destructor "
        "    Console.WriteLine(\"dtor=\" + this.Message); "
        "  end destructor; "
        "end ExD; "
        "class C public static method Main(): int "
        "  using var d := new ExD(\"ee\"); "
        "  Console.WriteLine(\"alive\"); "
        "  return 0; "
        "end Main; end C;", "alive\ndtor=ee"));
}

// §23: both property forms — expression-bodied and block-bodied. Pre-fix each
// dropped its access site's whole statement with no diagnostic. These are the
// cases the abandoned enclosing-type recovery could NOT reach, because the
// accessor body is inlined at the access site.
BEZEL_TEST(codegen_this_exception_message_2972, qualified_message_in_property_accessors_spec_23) {
    BEZEL_EXPECT_TRUE(prints(
        prog("  public property Msg: string => this.Message; "
             "  public property MsgBlock: string "
             "    get => this.Message; "
             "  end MsgBlock; ",
             "  using var e := new ExT(\"wxyz\"); "
             "  Console.WriteLine(\"p=\" + e.Msg); "
             "  Console.WriteLine(\"b=\" + e.MsgBlock); "),
        "p=wxyz\nb=wxyz"));
}

// §27: a lambda body inside an instance method — `this` is reached through the
// closure. Pre-fix the lambda returned 0.
BEZEL_TEST(codegen_this_exception_message_2972, qualified_message_in_lambda_body_spec_27) {
    BEZEL_EXPECT_TRUE(prints(
        prog("  public method ViaLambda(): int "
             "    var f := lambda() => this.Message.Length; "
             "    return f(); "
             "  end ViaLambda; ",
             "  using var e := new ExT(\"wxyz\"); "
             "  Console.WriteLine(\"lam=\" + e.ViaLambda().ToString()); "),
        "lam=4"));
}

// §8.5 string interpolation over the same access. Written first as a "worked
// pre-fix" control on the strength of a probe run — which was measured against
// a build that ALREADY carried an earlier draft of the fix. The fail-without
// run refuted that: it fails on 618c0c8 like every other frame. Kept, and
// re-labelled, as the standing reminder that a control must be measured on the
// base commit, not on the branch that is being validated.
BEZEL_TEST(codegen_this_exception_message_2972, qualified_message_in_interpolation_spec_8_5) {
    BEZEL_EXPECT_TRUE(prints(
        prog("  public method Interp(): string "
             "    return $\"i={this.Message}\"; "
             "  end Interp; ",
             "  using var e := new ExT(\"wxyz\"); "
             "  Console.WriteLine(e.Interp()); "),
        "i=wxyz"));
}

// ---------------------------------------------------------------------------
// The receiver axis: `base.`, a deeper chain, and the ArgumentException branch.
// ---------------------------------------------------------------------------

// §12.3: `base.Message` where the class base is itself a user exception
// subclass — the one `base` spelling sema admits here today (with a built-in
// base directly, `base.Message` is rejected before codegen; that reject is
// filed separately). It hit the identical #2491 degrade pre-fix.
BEZEL_TEST(codegen_this_exception_message_2972, base_qualified_message_through_user_base_spec_12_3) {
    BEZEL_EXPECT_TRUE(prints(
        "module M; "
        "class ExT: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end ExT; "
        "class ExT2: ExT "
        "  public constructor(m: string) base(m); end constructor; "
        "  public method ViaBase(): string return base.Message; end ViaBase; "
        "end ExT2; "
        "class C public static method Main(): int "
        "  using var l := new ExT2(\"bb\"); "
        "  Console.WriteLine(\"base=\" + l.ViaBase()); "
        "  return 0; "
        "end Main; end C;", "base=bb"));
}

// A two-step user chain read through `this.` from the deeper subclass — §12.1
// inheritance, and the classStruct rule that byte offset 8 is uniform across
// the whole user-exception subtree.
BEZEL_TEST(codegen_this_exception_message_2972, qualified_message_in_deeper_subclass_spec_12_1) {
    BEZEL_EXPECT_TRUE(prints(
        "module M; "
        "class ExT: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end ExT; "
        "class ExT2: ExT "
        "  public constructor(m: string) base(m); end constructor; "
        "  public method Deep(): string return this.Message; end Deep; "
        "end ExT2; "
        "class C public static method Main(): int "
        "  using var d := new ExT2(\"dd\"); "
        "  Console.WriteLine(\"deep=\" + d.Deep()); "
        "  return 0; "
        "end Main; end C;", "deep=dd"));
}

// §31.4's `ArgumentException` branch, whose subclass chains the two-argument
// `base(message, paramName)` — the Message slot must still be the one read.
BEZEL_TEST(codegen_this_exception_message_2972, qualified_message_in_argument_exception_subclass_spec_31_4) {
    BEZEL_EXPECT_TRUE(prints(
        "module M; "
        "class ArgSub: ArgumentException "
        "  public constructor(m: string, p: string) base(m, p); end constructor; "
        "  public method QualMsg(): string return this.Message; end QualMsg; "
        "end ArgSub; "
        "class C public static method Main(): int "
        "  using var a := new ArgSub(\"mm\", \"pp\"); "
        "  Console.WriteLine(\"arg=\" + a.QualMsg()); "
        "  return 0; "
        "end Main; end C;", "arg=mm"));
}

// The receiver was never the issue from OUTSIDE the class — that path already
// worked. Kept as the control the fix must not regress.
BEZEL_TEST(codegen_this_exception_message_2972, control_message_from_outside_the_class_spec_31_4) {
    BEZEL_EXPECT_TRUE(prints(
        prog("", "  using var l: ExT := new ExT(\"zz\"); "
                 "  Console.WriteLine(\"out=\" + l.Message); "),
        "out=zz"));
}

// ---------------------------------------------------------------------------
// The gate's negative side: a class whose OWN `Message` member exists must read
// that member, never the §31.4 header. Both halves — with and without a
// built-in exception ancestor — because the header-bearing one is the case a
// gate keyed on the member NAME alone would get wrong.
// ---------------------------------------------------------------------------

BEZEL_TEST(codegen_this_exception_message_2972, own_message_member_wins_on_plain_class_spec_11_3) {
    BEZEL_EXPECT_TRUE(prints(
        "module M; "
        "class Plain "
        "  private _m: string := \"own\"; "
        "  public property Message: string => _m; "
        "  public method Read(): string return this.Message; end Read; "
        "end Plain; "
        "class C public static method Main(): int "
        "  using var p := new Plain(); "
        "  Console.WriteLine(\"plain=\" + p.Read()); "
        "  return 0; "
        "end Main; end C;", "plain=own"));
}

BEZEL_TEST(codegen_this_exception_message_2972, own_message_member_wins_on_exception_subclass_spec_11_3) {
    BEZEL_EXPECT_TRUE(prints(
        "module M; "
        "class ExOwn: Exception "
        "  private _m: string := \"shadow\"; "
        "  public property Message: string => _m; "
        "  public method Read(): string return this.Message; end Read; "
        "end ExOwn; "
        "class C public static method Main(): int "
        "  using var p := new ExOwn(); "
        "  Console.WriteLine(\"shadow=\" + p.Read()); "
        "  return 0; "
        "end Main; end C;", "shadow=shadow"));
}

// ---------------------------------------------------------------------------
// Memory: the read hands out a BORROW of the header's string, exactly as the
// bare spelling does — no retain at the read, no release by the reader. Under
// ASan/LSan a double-release or a leak flips `ran` to false.
//
// The OUTPUT is asserted here too, not just `ran`/`exitCode`. A first version
// asserted only those two and PASSED without the fix: `Console.WriteLine` of
// the degraded null string neither crashed nor leaked, it just printed nothing
// — a vacuous pin. Concatenating the result (the shape that faulted in #2972's
// own repro) plus asserting the bytes makes it fail without the fix like every
// other case here, while keeping the sanitizer net.
// ---------------------------------------------------------------------------

BEZEL_TEST(codegen_this_exception_message_2972, qualified_message_is_memory_clean_spec_28_9) {
    auto r = bezel::test::runAotChecked(
        prog("  public method Bare(): string return Message; end Bare; "
             "  public method Qualified(): string return this.Message; "
             "  end Qualified; ",
             "  using var l: ExT := new ExT(\"zz\"); "
             "  Console.WriteLine(\"b=\" + l.Bare()); "
             "  Console.WriteLine(\"q=\" + l.Qualified()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("b=zz\nq=zz\n"));
}
