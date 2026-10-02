// Codegen / refcounting — issue #5253: `string?` residual SEGVs after #5245.
//
// Spec: §4.6 (`string`), §28.9 (runtime-managed types: a value is never freed
// while reachable), §22.3 (parameter modifiers: `out` is copy-out on normal
// return, the callee definitely assigns it), §7.9 (definite assignment of a
// no-initializer local), §6.6 (`??=`). Spec revision v0.313.
//
// The rows below are the issue's own probes (r25 gateB p5245 / rv5245), each
// loop-amplified to 100 iterations, building every string at RUNTIME so the
// refcount is real, under runAotChecked (generated-code ASan + LeakSanitizer:
// a SEGV, use-after-free, over-release abort or leak makes `ran` false). The
// oracle is the full stdout, so a value read from freed memory also fails.
//
// THE FIVE SHAPES (issue table) and their roots:
//   1. A `string?` PARAMETER assigned a field borrow (p_param), plus the
//      issue's producer / null-reassign siblings (s1, s2). A by-value
//      parameter's slot holds the CALLER's reference; retainAssignedStringParams
//      gave an assigned non-nullable parameter its own +1 at bind (#2696), but
//      not a `string?` one, so `p := c.Val` plain-stored a borrow that the next
//      `c.Set(...)` freed. Fix: the same bind retain for `string?`, the
//      drop-on-overwrite release admitted for it, and a scope-end release of
//      the final value when the parameter provably never escapes (which also
//      closes #2696's per-call leak for the non-escaping case).
//   2. `out r: string?` assigned a field borrow (s4) and
//   3. `out var s` then `s := c.Val` (s4b): the out-store and the recipient's
//      scope-end release now cover `string?`, and a `string?` out-var
//      recipient's slot is known to own its value, so `s := …` retains the new
//      value and releases the old.
//   4. A no-initializer `string?` local (s10): it was never a candidate of the
//      string-release pre-pass, so its `:=` plain-stored a borrow. It is now a
//      borrow candidate (null slot, retain-new/release-old, scope-end release
//      when non-escaping). Its non-nullable twin leaked one string per write.
//   5. `n ?? p` over a +1 producer local (n1): the consumer treated `p`'s
//      reference as transferred while `p`, non-escaping, also released it at
//      scope end. A non-escaping producer local no longer hands over its +1.
//
// HAZARD ROWS (same mechanism, found while fixing): a by-value `string` /
// `string?` parameter passed `out` (the callee's store released the caller's
// reference: "refcount over-release (string)"), and `r ??= c.Val` into an
// `out string?` parameter (stored a borrow the caller's recipient released).

#include <string>

#include "support/aot_support.hpp"   // runAotChecked
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

constexpr int kIters = 100;

// `C.Val` reads the field; `C.Set` stores over it (releasing the old string).
const char* kHdr =
    "module M; "
    "public class C "
    "  private _v: string; "
    "  public constructor(v: string) _v := v; end constructor; "
    "  public property Val: string => _v; "
    "  public method Set(x: string) _v := x; end Set; "
    "end C; ";

std::string prog(const std::string& body) { return std::string(kHdr) + body; }

std::string run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

std::string n(int i) { return std::to_string(i); }

// The value `c.Val` held before iteration i's first `c.Set(tag + i)`.
std::string prevVal(int i, const char* tag) {
    return i == 0 ? std::string("a1") : std::string(tag) + n(i - 1);
}

}  // namespace

// ---- Shape 1: `string?` parameter ------------------------------------------

// Issue probe p_param: the parameter is assigned a field borrow, the field is
// stored over, then the parameter is read. Pre-fix: SEGV.
BEZEL_TEST(codegen_string_optional_residual_5253,
           shape1_nullable_param_assigned_field_borrow_spec_22_3_28_9) {
    std::string want;
    for (int i = 0; i < kIters; ++i) want += "q" + n(i) + "\n";
    BEZEL_EXPECT_EQ(run(prog(
        "public class Program "
        "  static method Rep(c: C, p: string?, i: int) "
        "    c.Set(\"q\" + i.ToString()); "
        "    p := c.Val; "
        "    c.Set(\"r\" + i.ToString()); "
        "    Console.WriteLine(p ?? \"none\"); "
        "  end Rep; "
        "  public static method Main(): int "
        "    using var c := new C(\"a\" + 1.ToString()); "
        "    for var i in 0..<100 do "
        "      Rep(c, c.Val, i); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;")), want);
}

// Issue probe s1: producer reassigns of a `string?` parameter. Pre-fix: leak
// (400 allocations per 100 calls).
BEZEL_TEST(codegen_string_optional_residual_5253,
           shape1_nullable_param_producer_reassign_spec_22_3_28_9) {
    // Length of "y<i>": 2 for i < 10, 3 otherwise.
    BEZEL_EXPECT_EQ(run(prog(
        "public class Program "
        "  static method Rep(p: string?, i: int): int "
        "    p := \"x\" + i.ToString(); "
        "    p := \"y\" + i.ToString(); "
        "    return (p ?? \"\").Length; "
        "  end Rep; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0..<100 do "
        "      t := t + Rep(\"in\" + i.ToString(), i); "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;")), std::string("290\n"));
}

// Guard (passes on the base too): the #5245 reviewer's s2 probe — a `string?`
// parameter reassigned from another parameter, null, and back. The fix routes
// every one of these writes through retain-new / release-old.
BEZEL_TEST(codegen_string_optional_residual_5253,
           guard_nullable_param_param_null_reassign_spec_22_3_28_9) {
    // Length of "zz<i>": 3 for i < 10, 4 otherwise.
    BEZEL_EXPECT_EQ(run(prog(
        "public class Program "
        "  static method Rep(p: string?, q: string?, i: int): int "
        "    p := q; "
        "    p := null; "
        "    p := q; "
        "    return (p ?? \"\").Length; "
        "  end Rep; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0..<100 do "
        "      var a := \"in\" + i.ToString(); "
        "      var b := \"zz\" + i.ToString(); "
        "      t := t + Rep(a, b, i); "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;")), std::string("390\n"));
}

// Non-nullable twin of p_param: no crash before (#2696's retain), but the
// parameter's final value leaked once per call. Now released at scope end.
BEZEL_TEST(codegen_string_optional_residual_5253,
           shape1_nonnull_param_assigned_field_borrow_released_spec_28_9) {
    std::string want;
    for (int i = 0; i < kIters; ++i) want += "q" + n(i) + "\n";
    BEZEL_EXPECT_EQ(run(prog(
        "public class Program "
        "  static method Rep(c: C, p: string, i: int) "
        "    c.Set(\"q\" + i.ToString()); "
        "    p := c.Val; "
        "    c.Set(\"r\" + i.ToString()); "
        "    Console.WriteLine(p); "
        "  end Rep; "
        "  public static method Main(): int "
        "    using var c := new C(\"a\" + 1.ToString()); "
        "    for var i in 0..<100 do "
        "      Rep(c, c.Val, i); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;")), want);
}

// `p ??= c.Val` into a `string?` parameter: the stored borrow must take its
// own +1 (the slot owns its value), released at scope end.
BEZEL_TEST(codegen_string_optional_residual_5253,
           shape1_nullable_param_coalesce_assign_spec_6_6_28_9) {
    std::string want;
    for (int i = 0; i < kIters; ++i)
        want += prevVal(i, "r") + "\n" + "k" + n(i) + "\n";
    BEZEL_EXPECT_EQ(run(prog(
        "public class Program "
        "  static method Rep(c: C, p: string?, i: int) "
        "    p ??= c.Val; "
        "    c.Set(\"r\" + i.ToString()); "
        "    Console.WriteLine(p ?? \"none\"); "
        "  end Rep; "
        "  public static method Main(): int "
        "    using var c := new C(\"a\" + 1.ToString()); "
        "    for var i in 0..<100 do "
        "      Rep(c, null, i); "
        "      Rep(c, \"k\" + i.ToString(), i); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;")), want);
}

// A conditionally-assigned `string?` parameter and a `+=`-accumulated
// `string` parameter: both retained at bind, both released at scope end.
BEZEL_TEST(codegen_string_optional_residual_5253,
           shape1_param_conditional_and_concat_released_spec_4_6_28_9) {
    // Acc: len("s<i>") + 3. AccN: len of c.Val (even i) or "v<i>" (odd i).
    BEZEL_EXPECT_EQ(run(prog(
        "public class Program "
        "  static method Acc(p: string, k: int): int "
        "    for var j in 0..<k do "
        "      p += j.ToString(); "
        "    end; "
        "    return p.Length; "
        "  end Acc; "
        "  static method AccN(p: string?, c: C, i: int): int "
        "    if i mod 2 = 0 then "
        "      p := c.Val; "
        "    end; "
        "    c.Set(\"w\" + i.ToString()); "
        "    return (p ?? \"\").Length; "
        "  end AccN; "
        "  public static method Main(): int "
        "    using var c := new C(\"a\" + 1.ToString()); "
        "    var t := 0; "
        "    for var i in 0..<100 do "
        "      t := t + Acc(\"s\" + i.ToString(), 3) + "
        "               AccN(\"v\" + i.ToString(), c, i); "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;")), std::string("879\n"));
}

// ---- Shapes 2 and 3: `out` ----------------------------------------------------

// Issue probe s4: `out r: string?` assigned a field borrow, a producer, and a
// field borrow again; the field is then stored over in the caller.
BEZEL_TEST(codegen_string_optional_residual_5253,
           shape2_nullable_out_param_assigned_field_borrow_spec_22_3_28_9) {
    std::string want;
    for (int i = 0; i < kIters; ++i) want += prevVal(i, "q") + "\n";
    BEZEL_EXPECT_EQ(run(prog(
        "public class Program "
        "  static method Fill(out r: string?, c: C, i: int) "
        "    r := c.Val; "
        "    r := \"m\" + i.ToString(); "
        "    r := c.Val; "
        "  end Fill; "
        "  public static method Main(): int "
        "    using var c := new C(\"a\" + 1.ToString()); "
        "    for var i in 0..<100 do "
        "      var s: string? := null; "
        "      Fill(out s, c, i); "
        "      c.Set(\"q\" + i.ToString()); "
        "      Console.WriteLine(s ?? \"none\"); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;")), want);
}

// Issue probe s4b: an `out var s` recipient reassigned a field borrow.
BEZEL_TEST(codegen_string_optional_residual_5253,
           shape3_nullable_out_var_reassigned_field_borrow_spec_22_4_28_9) {
    std::string want;
    for (int i = 0; i < kIters; ++i) want += prevVal(i, "q") + "\n";
    BEZEL_EXPECT_EQ(run(prog(
        "public class Program "
        "  static method Fill(out r: string?, c: C, i: int) "
        "    r := \"m\" + i.ToString(); "
        "  end Fill; "
        "  public static method Main(): int "
        "    using var c := new C(\"a\" + 1.ToString()); "
        "    for var i in 0..<100 do "
        "      Fill(out var s, c, i); "
        "      s := c.Val; "
        "      c.Set(\"q\" + i.ToString()); "
        "      Console.WriteLine(s ?? \"none\"); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;")), want);
}

// `r ??= c.Val` into an `out string?` parameter: the stored borrow must take
// its own +1, because the caller's `out var` recipient releases it.
BEZEL_TEST(codegen_string_optional_residual_5253,
           hazard_out_param_coalesce_assign_field_borrow_spec_6_6_22_3) {
    std::string want;
    for (int i = 0; i < kIters; ++i) want += prevVal(i, "q") + "\n";
    BEZEL_EXPECT_EQ(run(prog(
        "public class Program "
        "  static method Fill(out r: string?, c: C) "
        "    r := null; "
        "    r ??= c.Val; "
        "  end Fill; "
        "  public static method Main(): int "
        "    using var c := new C(\"a\" + 1.ToString()); "
        "    for var i in 0..<100 do "
        "      Fill(out var s, c); "
        "      c.Set(\"q\" + i.ToString()); "
        "      Console.WriteLine(s ?? \"none\"); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;")), want);
}

// A by-value `string?` parameter passed `out`: the callee's store released
// the parameter's old value — the CALLER's reference. Pre-fix: abort,
// "refcount over-release (string)".
BEZEL_TEST(codegen_string_optional_residual_5253,
           hazard_nullable_param_passed_out_spec_22_3_28_9) {
    std::string want;
    for (int i = 0; i < kIters; ++i)
        want += "m" + n(i) + "\n" + "q" + n(i) + "\n";
    BEZEL_EXPECT_EQ(run(prog(
        "public class Program "
        "  static method Fill(out r: string?, i: int) "
        "    r := \"m\" + i.ToString(); "
        "  end Fill; "
        "  static method G(c: C, p: string?, i: int) "
        "    Fill(out p, i); "
        "    Console.WriteLine(p ?? \"none\"); "
        "  end G; "
        "  public static method Main(): int "
        "    using var c := new C(\"a\" + 1.ToString()); "
        "    for var i in 0..<100 do "
        "      G(c, c.Val, i); "
        "      c.Set(\"q\" + i.ToString()); "
        "      Console.WriteLine(c.Val); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;")), want);
}

// Non-nullable twin of the row above (aborted identically pre-fix).
BEZEL_TEST(codegen_string_optional_residual_5253,
           hazard_nonnull_param_passed_out_spec_22_3_28_9) {
    std::string want;
    for (int i = 0; i < kIters; ++i)
        want += "m" + n(i) + "\n" + "q" + n(i) + "\n";
    BEZEL_EXPECT_EQ(run(prog(
        "public class Program "
        "  static method Fill(out r: string, i: int) "
        "    r := \"m\" + i.ToString(); "
        "  end Fill; "
        "  static method G(c: C, p: string, i: int) "
        "    Fill(out p, i); "
        "    Console.WriteLine(p); "
        "  end G; "
        "  public static method Main(): int "
        "    using var c := new C(\"a\" + 1.ToString()); "
        "    for var i in 0..<100 do "
        "      G(c, c.Val, i); "
        "      c.Set(\"q\" + i.ToString()); "
        "      Console.WriteLine(c.Val); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;")), want);
}

// Passed `out` twice: the second store releases the first store's value.
BEZEL_TEST(codegen_string_optional_residual_5253,
           hazard_nullable_param_passed_out_twice_spec_22_3_28_9) {
    // Length of "m<i+1>": 2 for i + 1 < 10, 4 for i + 1 = 100, 3 otherwise.
    BEZEL_EXPECT_EQ(run(prog(
        "public class Program "
        "  static method Fill(out r: string?, i: int) "
        "    r := \"m\" + i.ToString(); "
        "  end Fill; "
        "  static method G(p: string?, i: int): int "
        "    Fill(out p, i); "
        "    Fill(out p, i + 1); "
        "    return (p ?? \"\").Length; "
        "  end G; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0..<100 do "
        "      t := t + G(\"q\" + i.ToString(), i); "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;")), std::string("292\n"));
}

// Guard: a parameter passed `ref` to a callee that only reads it is NOT
// retained (a `ref` argument is not counted as a write), so nothing leaks.
BEZEL_TEST(codegen_string_optional_residual_5253,
           guard_nullable_param_passed_ref_readonly_no_leak_spec_22_3) {
    // Length of "ab<i>": 3 for i < 10, 4 otherwise.
    BEZEL_EXPECT_EQ(run(prog(
        "public class Program "
        "  static method Peek(ref r: string?): int "
        "    return (r ?? \"\").Length; "
        "  end Peek; "
        "  static method Rep(p: string?): int "
        "    return Peek(ref p); "
        "  end Rep; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0..<100 do "
        "      t := t + Rep(\"ab\" + i.ToString()); "
        "    end; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;")), std::string("390\n"));
}

// ---- Shape 4: no-initializer local -------------------------------------------

// Issue probe s10. Pre-fix: SEGV.
BEZEL_TEST(codegen_string_optional_residual_5253,
           shape4_nullable_noinit_local_assigned_field_borrow_spec_7_9_28_9) {
    std::string want;
    for (int i = 0; i < kIters; ++i) want += "q" + n(i) + "\n";
    BEZEL_EXPECT_EQ(run(prog(
        "public class Program "
        "  public static method Main(): int "
        "    using var c := new C(\"a\" + 1.ToString()); "
        "    for var i in 0..<100 do "
        "      var s: string?; "
        "      s := c.Val; "
        "      c.Set(\"q\" + i.ToString()); "
        "      s := c.Val; "
        "      c.Set(\"r\" + i.ToString()); "
        "      Console.WriteLine(s ?? \"none\"); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;")), want);
}

// Non-nullable twin of s10. Pre-fix: leak (one string per write).
BEZEL_TEST(codegen_string_optional_residual_5253,
           shape4_nonnull_noinit_local_assigned_field_borrow_spec_7_9_28_9) {
    std::string want;
    for (int i = 0; i < kIters; ++i) want += "q" + n(i) + "\n";
    BEZEL_EXPECT_EQ(run(prog(
        "public class Program "
        "  public static method Main(): int "
        "    using var c := new C(\"a\" + 1.ToString()); "
        "    for var i in 0..<100 do "
        "      var s: string; "
        "      s := c.Val; "
        "      c.Set(\"q\" + i.ToString()); "
        "      s := c.Val; "
        "      c.Set(\"r\" + i.ToString()); "
        "      Console.WriteLine(s); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;")), want);
}

// A no-initializer `string?` local filled by a pre-declared `out` argument,
// then (in Get) reassigned a field borrow and returned.
BEZEL_TEST(codegen_string_optional_residual_5253,
           shape4_nullable_noinit_local_out_and_return_spec_22_3_28_9) {
    std::string want;
    for (int i = 0; i < kIters; ++i)
        want += "m" + n(i) + "\n" + prevVal(i, "z") + "\n";
    BEZEL_EXPECT_EQ(run(prog(
        "public class Program "
        "  static method Fill(out r: string?, i: int) "
        "    r := \"m\" + i.ToString(); "
        "  end Fill; "
        "  static method Get(c: C, i: int): string? "
        "    var s: string?; "
        "    Fill(out s, i); "
        "    s := c.Val; "
        "    c.Set(\"z\" + i.ToString()); "
        "    return s; "
        "  end Get; "
        "  public static method Main(): int "
        "    using var c := new C(\"a\" + 1.ToString()); "
        "    for var i in 0..<100 do "
        "      var s: string?; "
        "      Fill(out s, i); "
        "      Console.WriteLine(s ?? \"none\"); "
        "      Console.WriteLine(Get(c, i) ?? \"none\"); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;")), want);
}

// ---- Shape 5: `??` over a producer local -------------------------------------

// Issue probe n1. Pre-fix: SEGV (double release of `p`'s reference).
BEZEL_TEST(codegen_string_optional_residual_5253,
           shape5_coalesce_over_producer_local_spec_4_6_28_9) {
    std::string want;
    for (int i = 0; i < kIters; ++i) want += "x" + n(i) + "\n";
    BEZEL_EXPECT_EQ(run(prog(
        "public class Program "
        "  static method Pick(n: string?, i: int): string "
        "    var p := \"x\" + i.ToString(); "
        "    var r := n ?? p; "
        "    return r; "
        "  end Pick; "
        "  public static method Main(): int "
        "    for var i in 0..<100 do "
        "      Console.WriteLine(Pick(null, i)); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;")), want);
}

// Guard (passes on the base too): the `string?`-result twin — both `??` arms
// nullable, the right one a producer local — with a null and a non-null left
// operand. Pins that the shape 5 fix did not need (and must not add) a mint on
// the nullable `??` path.
BEZEL_TEST(codegen_string_optional_residual_5253,
           guard_nullable_coalesce_over_producer_local_spec_4_6_28_9) {
    std::string want;
    for (int i = 0; i < kIters; ++i)
        want += "x" + n(i) + "\n" + "y" + n(i) + "\n";
    BEZEL_EXPECT_EQ(run(prog(
        "public class Program "
        "  static method Pick(n: string?, i: int): string? "
        "    var p: string? := \"x\" + i.ToString(); "
        "    var r := n ?? p; "
        "    return r; "
        "  end Pick; "
        "  public static method Main(): int "
        "    for var i in 0..<100 do "
        "      Console.WriteLine(Pick(null, i) ?? \"none\"); "
        "      Console.WriteLine(Pick(\"y\" + i.ToString(), i) ?? \"none\"); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;")), want);
}
