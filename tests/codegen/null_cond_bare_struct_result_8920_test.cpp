// Codegen / ASan — a null-conditional call `h?.M(...)` whose method returns
// a BARE value-type struct (an owning choice, a non-owning choice, a record)
// lifted the bare struct as if it were already the `C?` carrier. Issue #8920.
//
// Spec: §7.5 (null-conditional access — "`x?.M` returns null if `x` is null;
// otherwise it performs the access. The result type is always nullable"),
// §35.9 (Nullable `C?` — "`C?` is the ordinary nullable value type"; "For an
// owning `C`, the wrapper `C?` is itself move-only"), §28.3 (every owned
// value is disposed exactly once). Spec revision v0.359.
//
// THE DEFECT. `Emitter::lowerNullCondCall` decided "the callee already
// returns the nullable carrier" from the LIFTED call type (`C?`, which §7.5
// makes nullable for every `?.`) plus `retTy->isStructTy()`. A bare record or
// choice return is itself an LLVM struct, so the bare `C` passed through as
// the result: the binding's slot had the bare `C`'s width while every `C?`
// consumer read it as `{i8 hasValue; C}`.
//
// MEASURED on the base (r30-c1 @ a40ba4c2f):
//   * row A (the issue's repro) under ASan: `stack-buffer-overflow ... READ
//     of size 24` in main, at the binding's scope-end disposal;
//   * row B (a throwing callee): the same overflow;
//   * row C (a non-owning choice result inspected by `case`): `bezelc`
//     SEGFAULTS (exit 139);
//   * row D (a bare record result): `IR verification failed` — `icmp ne i32
//     %has.val, i8 0`, the record's first field read as `hasValue`.
//
// THE FIX. The callee returned the bare value exactly when the non-null
// inner of the call type lowers to the callee's LLVM return type; that
// return is now wrapped into `{i8 1, C}` like every other value return. The
// #7538 exemption of an owning-choice result from the call-site zero-store
// (made because the mis-lift left the taker unable to dispose a returned
// argument) is removed: with the lift right, the taker disposes it, and
// `ownership_owned_choice_arg_callee_throw_7538`'s `?.Pass(move s)` row pins
// that it is still disposed exactly once.
//
// ORACLE. `runAotChecked` (AOT + ASan/LSan when the sanitizer runs); each
// program loops 60-200 times and prints a count, so a lost or doubled
// disposal changes stdout as well as tripping the sanitizer.

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

std::string issueRepro() {
    return
        "module M; "
        "class Res "
        "public static N: int := 0; "
        "public constructor() end constructor; "
        "destructor Res.N := Res.N + 1; end destructor; "
        "end Res; "
        "choice OChoice Text(owned r: Res), Empty; "
        "end OChoice; "
        "class H "
        "public constructor() end constructor; "
        "public method TakeR(o: OChoice): OChoice return OChoice.Empty; end TakeR; "
        "end H; "
        "class Program "
        "public static method Main() "
        "var i := 0; "
        "while i < 200 do "
        "var h: H? := (if i mod 3 = 0 then null else new H()); "
        "var b := OChoice.Text(new Res()); "
        "var r := h?.TakeR(b); "
        "i := i + 1; "
        "end while; "
        "Console.WriteLine(Res.N); "
        "end Main; "
        "end Program; ";
}

std::string throwingCallee() {
    return
        "module M; "
        "class Res "
        "public static N: int := 0; "
        "public constructor() end constructor; "
        "destructor Res.N := Res.N + 1; end destructor; "
        "end Res; "
        "choice OChoice Text(owned r: Res), Empty; "
        "end OChoice; "
        "class H "
        "public constructor() end constructor; "
        "public method TakeR(o: OChoice, c: bool): OChoice "
        "if c then throw new InvalidOperationException(\"boom\"); end; "
        "return OChoice.Empty; "
        "end TakeR; "
        "end H; "
        "class Program "
        "public static method Main() "
        "var i := 0; "
        "while i < 200 do "
        "var h: H? := new H(); "
        "var b := OChoice.Text(new Res()); "
        "try "
        "var r := h?.TakeR(b, i mod 2 = 0); "
        "catch "
        "InvalidOperationException => () "
        "end try; "
        "i := i + 1; "
        "end while; "
        "Console.WriteLine(Res.N); "
        "end Main; "
        "end Program; ";
}

std::string plainChoice() {
    return
        "module M; "
        "choice PC Num(v: int), Empty; "
        "end PC; "
        "record R(a: int, b: int, c: int); "
        "class H "
        "public constructor() end constructor; "
        "public method GetC(k: int): PC return PC.Num(k); end GetC; "
        "public method GetR(k: int): R return new R(k, k + 1, k + 2); end GetR; "
        "end H; "
        "class Program "
        "public static method Main() "
        "var i := 0; "
        "var s := 0; "
        "var nulls := 0; "
        "while i < 60 do "
        "var h: H? := (if i mod 3 = 0 then null else new H()); "
        "var r := h?.GetC(i); "
        "case r of "
        "null => nulls := nulls + 1, "
        "PC.Num(v) => s := s + v, "
        "PC.Empty => () "
        "end case; "
        "i := i + 1; "
        "end while; "
        "Console.WriteLine(s); "
        "Console.WriteLine(nulls); "
        "end Main; "
        "end Program; ";
}

std::string bareRecord() {
    return
        "module M; "
        "choice PC Num(v: int), Empty; "
        "end PC; "
        "record R(a: int, b: int, c: int); "
        "class H "
        "public constructor() end constructor; "
        "public method GetC(k: int): PC return PC.Num(k); end GetC; "
        "public method GetR(k: int): R return new R(k, k + 1, k + 2); end GetR; "
        "end H; "
        "class Program "
        "public static method Main() "
        "var i := 0; "
        "var s := 0; "
        "var nulls := 0; "
        "while i < 60 do "
        "var h: H? := (if i mod 3 = 0 then null else new H()); "
        "var q := h?.GetR(i); "
        "if q = null then nulls := nulls + 100; else s := s + q!.c; end if; "
        "i := i + 1; "
        "end while; "
        "Console.WriteLine(s); "
        "Console.WriteLine(nulls); "
        "end Main; "
        "end Program; ";
}

std::string twins() {
    return
        "module M; "
        "class Res "
        "public static N: int := 0; "
        "public constructor() end constructor; "
        "destructor Res.N := Res.N + 1; end destructor; "
        "end Res; "
        "choice OChoice Text(owned r: Res), Empty; "
        "end OChoice; "
        "class H "
        "public constructor() end constructor; "
        "public method Make(): OChoice return OChoice.Text(new Res()); end Make; "
        "public method Pass(owned o: OChoice): OChoice return o; end Pass; "
        "public method MaybeMake(k: int): OChoice? "
        "if k mod 2 = 0 then return null; end if; "
        "return OChoice.Text(new Res()); "
        "end MaybeMake; "
        "end H; "
        "class Holder "
        "private _h: H?; "
        "public constructor(h: H?) _h := h; end constructor; "
        "public method Go(): OChoice? return _h?.Make(); end Go; "
        "end Holder; "
        "class Program "
        "public static method Main() "
        "var i := 0; "
        "var texts := 0; "
        "while i < 200 do "
        "var h: H? := (if i mod 3 = 0 then null else new H()); "
        "var a := h?.Make(); "
        "case a of "
        "null => (), "
        "OChoice.Text(_) => texts := texts + 1, "
        "OChoice.Empty => () "
        "end case; "
        "var s := OChoice.Text(new Res()); "
        "var z := h?.Pass(move s); "
        "var d := h?.Make() ?? OChoice.Empty; "
        "var m := h?.MaybeMake(i); "
        "var hd := new Holder(h); "
        "var g := hd.Go(); "
        "i := i + 1; "
        "end while; "
        "Console.WriteLine(texts); "
        "Console.WriteLine(Res.N); "
        "end Main; "
        "end Program; ";
}

}  // namespace

// A — the issue's own repro: an owning-choice result, a receiver null on one
// iteration in three, 200 iterations. Every `Res` is disposed once: 200.
BEZEL_TEST(codegen_8920_null_cond_bare_struct_result,
           owning_choice_result_on_sometimes_null_receiver_spec_35_9) {
    auto r = runAotChecked(issueRepro());
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n");
}

// B — the issue's throwing-callee twin: `TakeR` throws on every second call,
// so the binding is left on the exception edge; still 200 disposals.
BEZEL_TEST(codegen_8920_null_cond_bare_struct_result,
           owning_choice_result_throwing_callee_spec_35_9) {
    auto r = runAotChecked(throwingCallee());
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n");
}

// C — a NON-owning choice result inspected by a `case` with a `null` arm
// (§35.9: a `C?` scrutinee adds the explicit-`null`-arm requirement). 60
// iterations, 20 null receivers: sum of the non-null `i` is 1200.
BEZEL_TEST(codegen_8920_null_cond_bare_struct_result,
           value_like_choice_result_case_null_arm_spec_35_9) {
    auto r = runAotChecked(plainChoice());
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1200\n20\n");
}

// D — a bare RECORD result (§7.5: "The result type is always nullable"):
// sum of `c = i + 2` over the 40 non-null receivers is 1280; 20 nulls.
BEZEL_TEST(codegen_8920_null_cond_bare_struct_result,
           record_result_on_sometimes_null_receiver_spec_7_5) {
    auto r = runAotChecked(bareRecord());
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1280\n2000\n");
}

// E — the twins in one loop: a `case` over an owning `C?` result, a
// `?.Pass(move s)` that returns its argument, a `??` default after `?.`, a
// method that already returns `C?` (passed through unwrapped), and `?.` on a
// FIELD receiver. 133 non-null receivers; disposals 133 + 200 + 133 + 67 +
// 133 = 666.
BEZEL_TEST(codegen_8920_null_cond_bare_struct_result,
           owning_choice_result_twins_spec_35_9) {
    auto r = runAotChecked(twins());
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "133\n666\n");
}

// F — issue #8558's repro, verbatim: a bare record result followed by `??`
// (§7.5, then §7.7's null-coalescing operator). On the base `bezelc` failed IR
// verification (`icmp ne i32 %has.val, i8 0`; a PHI operand type mismatch).
// Expected 3 + 4 = 7.
BEZEL_TEST(codegen_8920_null_cond_bare_struct_result,
           issue_8558_record_result_then_coalesce_spec_7_5) {
    auto r = runAotChecked(
        "module P; "
        "record Pt(X: int, Y: int); "
        "public class Helper "
        "  public method Mk(): Pt "
        "    return new Pt(3, 4); "
        "  end Mk; "
        "end Helper; "
        "public class Program "
        "  public static method Main() "
        "    var h: Helper? := new Helper(); "
        "    var r: Pt := h?.Mk() ?? new Pt(0, 0); "
        "    Console.WriteLine(r.X + r.Y); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}
