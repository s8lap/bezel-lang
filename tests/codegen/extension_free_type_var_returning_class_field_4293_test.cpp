// Codegen — issue #4293 (§26.4 / §26.5 / §14.1, v0.298): a generic extension
// method on a user-declared generic class — `extension Box<W>` — whose declared
// return type is the extension's own free type variable, AND whose body reads
// that free type variable THROUGH the receiver (e.g. `this._v`), previously
// silently miscompiled for ANY binding of `W` whose lowered type was wider
// than i32 (every reference type: `string`, classes, interfaces).
//
//   * For `W = string`, the body's `this._v` loaded 4 bytes of the 64-bit
//     string pointer; `Console.WriteLine(s)` then printed "0" — the issue's
//     first reproducer.
//   * For the same case with a follow-up `s.Length`, the 4-byte truncation
//     became a load-of-i32-into-ptr-typed-GEP and the IR verifier aborted with
//     "GEP base pointer is not a vector or a vector of pointers / Load operand
//     must be a pointer." — the issue's second reproducer.
//   * For `W = int` (value type, exactly i32 wide), the bug is invisible: the
//     field's storage was incorrectly laid out as i32 (the §14.1 placeholder
//     default) but the int value also fits, so the round-trip produced the
//     expected `42`. This is NOT a regression case — the value-typed binding
//     survives the §14.1 placeholder by accident.
//
// MECHANISM. §26.5 / #714 binds the extension's free type variables (here
// `W`) at the call site via `lookupExtensionMember`'s recorded binding
// (`ctx_.exprExtBinding`), keyed on the call's MemberAccessExpr. Codegen reads
// that binding at LlvmBackend_Calls2.cpp ~L11148 and installs `genericSubst_[W]
// = ptrTy()` so a body that mentions `W` directly (a parameter type, a return
// type, or a method-local declared as `W`) lowers through `lowerTypeRef` to
// the concrete LLVM type.
//
// What the binding does NOT cover is the RECEIVER CLASS's OWN type variable
// (`Box<U>`'s `U`). Reading `this._v` accesses `_v: U` — a NAMED-with-no-
// declaration type ref whose name is `U`, not `W`. `lowerTypeRef` looks the
// name up in `genericSubst_`, doesn't find it (only `W` is bound), falls
// through, and returns the i32 placeholder. `classStruct` uses the same
// substitution for the receiver's field layout, so the field is laid out at
// the placeholder width (4 bytes) — the field load produces 4 bytes where a
// `ptr` was expected, and the surface symptoms above follow.
//
// The fix is symmetric: the codegen call site also extracts the receiver
// CLASS's free variables from the receiver's `genericArgs` against the
// receiver class's `genericParams`, and installs them into `genericSubst_` /
// `genericSubstSemaTy_` ALONGSIDE the extension's free var binding (a
// distinct name space — `U` vs `W` here — so there is no collision even when
// they happen to spell the same).
//
// Each test below exercises one shape that the bug breaks. Both must FAIL
// (silent wrong value / IR-verifier crash) without the fix and PASS with it.
//
// REGRESSION GUARDS:
//   * `class_returning_free_var_through_receiver_field_string_4293` — the
//     exact issue reproducer A: `Box<string>`, `extension Box<W>`,
//     `Peek(): W => this._v`. Asserts the program runs and prints
//     `"hello"` (and does NOT print `"0"`).
//   * `class_returning_free_var_through_receiver_field_string_length_4293`
//     — the IR-verifier-crash reproducer B: same `Peek()` but the caller
//     reads `s.Length` instead of `Console.WriteLine(s)`. Asserts the
//     program compiles cleanly (no IR-verifier failure on stderr) AND
//     returns 5 (`"hello".Length`).
//   * `class_returning_free_var_through_receiver_field_class_type_4293` —
//     the class-reference twin: `Box<Widget>` where `Widget` is a user-
//     declared class. Pre-fix the field is laid out as i32 and the load
//     returns a truncated half-pointer; any later read of the Widget's
//     members lands on garbage. Asserts the stored `tag` value is read
//     back correctly (7), proving the receiver-class binding installed
//     `Widget`'s vtable-ptr layout for `_v`.
//   * `class_int_returning_free_var_value_case_still_works_4293` — control:
//     `Peek(): int => this._v` on `Box<int>`. The fix is ADDITIVE: the
//     value-typed case (the field type fits in i32) already worked by
//     accident on the unfixed tree and must keep working after the fix.
//     Asserts the program returns 42 (the stored value), demonstrating
//     the fix did not regress the placeholder-sized slot.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains,
                                            // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::JitResult;

namespace {

// True iff runAot's program ran to completion AND its stderr carries an
// IR-verifier rejection ("IR verification failed" / "GEP base pointer" /
// "Load operand must be a pointer"). The pre-fix program emits the verifier
// message on stderr; on a COMPILE-TIME CRASH (here: the verifier rejects the
// malformed IR before linking) the program does not run, `.ran` is false —
// we report that as a verifier fail too so the test names the actual defect.
bool emittedVerifierFailure(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("IR verification failed") != std::string::npos ||
            d.message.find("GEP base pointer") != std::string::npos ||
            d.message.find("Load operand must be a pointer") != std::string::npos)
            return true;
    return false;
}

bool printContains(const JitResult& r, const std::string& needle) {
    return r.ran && r.stdoutText.find(needle) != std::string::npos;
}

}  // namespace

// REPRO A — `string` binding: `Box<string>("hello")`, `Peek(): W => this._v`.
// Pre-fix: the body's `this._v` loads 4 bytes from the slot laid out as
// `i32`; Console.WriteLine coerces that to string and prints "0".
// Post-fix: prints "hello".
BEZEL_TEST(extension_free_type_var_returning_class_field_4293,
           class_returning_free_var_through_receiver_field_string_4293) {
    auto r = runAot(
        "module M; "
        "class Box<U> "
        "  public _v: U; "
        "  public constructor(v: U) _v := v; end; "
        "end Box; "
        "extension Box<W> "
        "  public method Peek(): W => this._v; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box<string>(\"hello\"); "
        "    var s := b.Peek(); "
        "    Console.WriteLine(s); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(printContains(r, "hello"));
    // The pre-fix silent-wrong-value prints "0" — the message we explicitly
    // want to NOT see. The post-fix program prints "hello" and exits.
    BEZEL_EXPECT_FALSE(printContains(r, "0\n"));
}

// REPRO B — IR-verifier crash shape: same as REPRO A but the caller reads
// `s.Length` (`string.Length` returns int). Pre-fix: the load of a 32-bit
// field into a 64-bit pointer-typed GEP triggers the IR verifier's
// "GEP base pointer is not a vector or a vector of pointers" and
// "Load operand must be a pointer" rejections, halting bezelc with exit 1.
// Post-fix: compiles cleanly, prints "5".
BEZEL_TEST(extension_free_type_var_returning_class_field_4293,
           class_returning_free_var_through_receiver_field_string_length_4293) {
    auto r = runAot(
        "module M; "
        "class Box<U> "
        "  public _v: U; "
        "  public constructor(v: U) _v := v; end; "
        "end Box; "
        "extension Box<W> "
        "  public method Peek(): W => this._v; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box<string>(\"hello\"); "
        "    var s := b.Peek(); "
        "    return s.Length; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(emittedVerifierFailure(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// REPRO D — class-reference twin: `Box<Widget>` where `Widget` is a
// user-declared class (vtable pointer). Pre-fix the field's slot is laid
// out at the i32 placeholder, so reading `_v` returns 4 bytes of the 8-byte
// vtable pointer; reading `tag` from the truncated half would land on
// garbage. Post-fix the field is the correct `ptr` layout and the stored
// `tag` value (7) reads back identically.
BEZEL_TEST(extension_free_type_var_returning_class_field_4293,
           class_returning_free_var_through_receiver_field_class_type_4293) {
    auto r = runAot(
        "module M; "
        "class Widget "
        "  public tag: int; "
        "  public constructor(t: int) tag := t; end; "
        "end Widget; "
        "class Box<U> "
        "  public _v: U; "
        "  public constructor(v: U) _v := v; end; "
        "end Box; "
        "extension Box<W> "
        "  public method Peek(): W => this._v; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box<Widget>(new Widget(7)); "
        "    var w := b.Peek(); "
        "    return w.tag; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(emittedVerifierFailure(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// CONTROL — the VALUE-TYPE binding (`Box<int>`) already works on the unfixed
// tree (the field's declared type is `U`, substituted via
// `genericSubst_[U] = i32()` by the existing class-method call-site binding at
// LlvmBackend_Calls2.cpp ~L10939, which the extension path used to inherit
// indirectly). The fix is ADDITIVE — it adds a binding for the receiver class's
// free var where there was none before; a working path must keep working.
// Asserting the exit code is the cleanest oracle: an int return makes the
// program observable without leaning on stdout text.
//
// The return is spelled `W`, the extension's own free variable, and the `int`
// is taken at the CALL SITE by an explicit §5.2 conversion. It cannot be
// spelled `Peek(): int => this._v`: §5.1 admits a type-parameter value only to
// `Object` and to the classes and interfaces its constraints guarantee, and `U`
// is unconstrained, so returning it as `int` is correctly a compile error
// ("`U` is a type parameter ... never to a primitive", §5.1 / §14.2). The
// row's subject is the int BINDING surviving the additive fix, which the
// `W` + `int(...)` spelling observes at the same substitution.
BEZEL_TEST(extension_free_type_var_returning_class_field_4293,
           class_int_returning_free_var_value_case_still_works_4293) {
    auto r = runAot(
        "module M; "
        "class Box<U> "
        "  public _v: U; "
        "  public constructor(v: U) _v := v; end; "
        "end Box; "
        "extension Box<W> "
        "  public method Peek(): W => this._v; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box<int>(42); "
        "    return int(b.Peek()); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}
