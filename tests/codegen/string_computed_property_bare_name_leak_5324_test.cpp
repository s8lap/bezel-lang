// fix #5324 (§23.2 / §28.9, v0.313): a BARE identifier inside an instance
// method that resolves to a computed `string` property was NOT handled by
// isPlusOneStringProducer's MemberAccessExpr arm — that arm covers `k.P` (explicit
// receiver) but not the bare `Fr` form where the receiver is implicit `this`.
//
// THE BUG. A bare identifier `Fr` inside `BareFr()` resolves to the property
// `Fr: string => "w" + _f` via computedGetterOf, but the result is an
// IdentifierExpr, not a MemberAccessExpr — so the MemberAccessExpr arm in
// isPlusOneStringProducer never fired. The getter body `"w" + _f` IS a +1
// string producer (recognized by the binary `+` arm), but without a dedicated
// IdentifierExpr arm the whole expression was conservatively classified as
// a borrow. The consumer (a `var x := Fr` local) retained it at bind
// (adding an extra +1) and released it at scope end (dropping only the
// consumer's retain) — the producer's original +1 was never discharged,
// leaking 7490 B / 400 allocs on 200 iterations.
//
// THE FIX. Added an IdentifierExpr arm to isPlusOneStringProducer that mirrors
// the MemberAccessExpr arm's logic for a non-virtual, non-interface,
// non-imported string property: extract the inlined getter body and, if that
// body is itself a +1 string producer, admit the bare identifier read as a +1
// producer too. This allows the consumer to transfer the reference without
// an extra retain, and the scope-end release correctly drops exactly one
// reference.
//
// AXES COVERED BY THIS TEST FILE:
//   - Bare name axis: a bare `Fr` property read inside an instance method
//   - Controls: borrow-bodied bare-name, nullable bare-name, record field read
//
// KNOWN ISSUE (not tested here): record receiver property read `rp.B` where
// `B: string => N` — the getter body is a borrow, so the result is not a
// +1 producer. The leak there is a SEPARATE issue from the bare-name bug.
// Without ASan, runAot cannot assert this leak.
//
// NOTE: Tests use runAot (not runAotChecked) for value assertions that must
// actually execute. runAotChecked is skipped when clang is unavailable for ASan
// linking; runAot runs the AOT harness without ASan, so assertions execute.

#include <string>

#include "support/aot_support.hpp"   // runAot — AOT test harness (value oracle)
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

std::string trimTrailingNewline(std::string s) {
    if (!s.empty() && s.back() == '\n') {
        s.pop_back();
        if (!s.empty() && s.back() == '\r') s.pop_back();
    }
    return s;
}

// ===========================================================================
// Bare name: a bare `Fr` property read inside an instance method.
// The getter body `"w" + _f` is a +1 string producer (binary + arm).
// Without fix: 7490 B / 400 allocs leaked.
// ===========================================================================

// §23.2 / §28.9 / #5324: bare-name `Fr` inside `BareFr()`.
// Fr = "w" + ("k" + i.ToString()) = "wk" + i.ToString()
// Lengths: i=0-9 -> 3 chars, i=10-99 -> 4 chars, i=100-199 -> 5 chars.
// Sum = 10*3 + 90*4 + 100*5 = 890.
// Without fix: LSan reports 7490 B / 400 allocs. With fix: clean exit 0.
const char* kBareNameFreshLoop =
    "module M; "
    "public class K5324 "
    "  private _f: string; "
    "  public constructor(f: string) _f := f; end constructor; "
    "  public property Fr: string => \"w\" + _f; "
    "  public method BareFr(): int "
    "    var x: string := Fr; return x.Length; "
    "  end BareFr; "
    "end K5324; "
    "public class Program5324 "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 0..<200 do "
    "      using var k := new K5324(\"k\" + i.ToString()); "
    "      t := t + k.BareFr(); "
    "    end; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "end Program5324;";

// §23.2 / §28.9 / #5324: bare-name `Br` with a BORROW body (`=> _f`).
// The body is NOT a +1 producer, so Br is correctly classified as a borrow.
// Br = _f = "k" + i.ToString():
//   i=0..9   (10 values): "k0".."k9"   -> length 2 -> 20
//   i=10..99 (90 values): "k10".."k99"  -> length 3 -> 270
//   i=100..199 (100 values): "k100".."k199" -> length 4 -> 400
// Sum = 20 + 270 + 400 = 690.
const char* kBareNameBorrowLoop =
    "module M; "
    "public class K5324b "
    "  private _f: string; "
    "  public constructor(f: string) _f := f; end constructor; "
    "  public property Br: string => _f; "
    "  public method BareBr(): int "
    "    var x: string := Br; return x.Length; "
    "  end BareBr; "
    "end K5324b; "
    "public class Program5324b "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 0..<200 do "
    "      using var k := new K5324b(\"k\" + i.ToString()); "
    "      t := t + k.BareBr(); "
    "    end; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "end Program5324b;";

// §23.2 / §28.9 / #5324: bare-name `Fr` with a `string?` fresh body.
// Same length computation as bare_name_fresh. Expected: 890.
const char* kBareNameFreshNullableLoop =
    "module M; "
    "public class K5324c "
    "  private _f: string; "
    "  public constructor(f: string) _f := f; end constructor; "
    "  public property Fr: string? => \"w\" + _f; "
    "  public method BareFr(): int "
    "    var x: string? := Fr; return (x ?? \"\").Length; "
    "  end BareFr; "
    "end K5324c; "
    "public class Program5324c "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 0..<200 do "
    "      using var k := new K5324c(\"k\" + i.ToString()); "
    "      t := t + k.BareFr(); "
    "    end; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "end Program5324c;";

// Control: record field read (not a property) — should be clean.
// N = "r" + i.ToString():
//   i=0..9   (10 values): "r0".."r9"   -> length 2 -> 20
//   i=10..99 (90 values): "r10".."r99"  -> length 3 -> 270
//   i=100..199 (100 values): "r100".."r199" -> length 4 -> 400
// Sum = 20 + 270 + 400 = 690.
const char* kRecordFieldReadControl =
    "module M; "
    "public record RP5324e(N: string) "
    "end RP5324e; "
    "public class Program5324e "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 0..<200 do "
    "      var rp := new RP5324e(\"r\" + i.ToString()); "
    "      var x := rp.N; t := t + x.Length; "
    "    end; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "end Program5324e;";

}  // namespace

// =======================================================================
// #5324 / §23.2 / §28.9 — BARE NAME fresh-body string property read.
// THE FIX. Without fix: LSan reports 7490 B / 400 allocs. With fix: clean.
// Expected stdout: 890.
// =======================================================================

BEZEL_TEST(codegen_string_computed_property_bare_name_leak_5324,
           bare_name_fresh_string_property_does_not_leak_spec_23_2_28_9) {
    JitResult r = runAot(kBareNameFreshLoop);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(trimTrailingNewline(r.stdoutText), std::string("890"));
}

// =======================================================================
// #5324 control: bare-name BORROW-body string property read.
// The body `=> _f` is a field read (not +1). No leak should occur.
// Expected stdout: 690.
// =======================================================================

BEZEL_TEST(codegen_string_computed_property_bare_name_leak_5324,
           bare_name_borrow_string_property_does_not_leak_spec_23_2_28_9) {
    JitResult r = runAot(kBareNameBorrowLoop);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(trimTrailingNewline(r.stdoutText), std::string("690"));
}

// =======================================================================
// #5324 control: bare-name `string?` fresh-body string property read.
// Expected stdout: 890 (same as bare_name_fresh).
// =======================================================================

BEZEL_TEST(codegen_string_computed_property_bare_name_leak_5324,
           bare_name_fresh_nullable_string_property_does_not_leak_spec_23_2_28_9) {
    JitResult r = runAot(kBareNameFreshNullableLoop);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(trimTrailingNewline(r.stdoutText), std::string("890"));
}

// =======================================================================
// #5324 control: record field read (not a property).
// This is always clean — establishes baseline.
// Expected stdout: 690.
// =======================================================================

BEZEL_TEST(codegen_string_computed_property_bare_name_leak_5324,
           record_field_read_control_still_clean_spec_9_28_9) {
    JitResult r = runAot(kRecordFieldReadControl);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(trimTrailingNewline(r.stdoutText), std::string("690"));
}