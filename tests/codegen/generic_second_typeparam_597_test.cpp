// Codegen — a generic method that returns a type INSTANTIATED with the SECOND
// (or any index >= 1) type parameter of the enclosing generic class (issue
// #597).
//
// §14.1 (generic declarations / monomorphization) / §22.4 (methods): for a
// class `Pair<A, B>`, a method `Swap(): Pair<B, A>` or `BoxB(): Box<B>` returns
// a type built from the SECOND class type parameter.  The result's layout is
// governed by that substituted result type — `Pair<int,string>.Swap()` returns
// `Pair<string,int>`, `Pair<int,string>.BoxB()` returns `Box<string>` — NOT the
// receiver's instantiation.
//
// Root cause (pre-fix): the substitution that maps a class type parameter to
// its concrete argument assumed an index-0 / first-parameter binding in three
// places, so a return/`new` type referencing the SECOND param resolved to the
// FIRST argument (or to the i32 placeholder, which equals `int` by luck — that
// is why a first-parameter return like `Box<A>` with A=int worked but the
// second did not):
//   (1) sema `substituteMemberType` returned a CONSTRUCTED return type
//       (`Box<B>`, `Pair<B,A>`) unchanged — it only substituted a BARE param
//       reference — so the call's result type kept the unresolved class params
//       and codegen defaulted each to i32.  Symptom B (zero-init).
//   (2) codegen recorded the RESULT local's generic substitution from the
//       method's raw declared return type (with bare, swapped params) /from the
//       receiver's subst, so `using var s := p.Swap()` laid `s` out as the
//       receiver type `Pair<int,string>` and `s._b` read the wrong field width
//       → malformed `ret ptr` in an i32 function.  Symptom A (IR-verify fail).
//   (3) codegen lowered the `new`-expression's constructor ARGUMENTS under the
//       NEW type's substitution rather than the enclosing scope's, so `_b`/`_a`
//       inside `new Pair<B,A>(_b, _a)` read `this` at the swapped layout
//       (returned garbage / segfaulted for record args).
//
// These tests must IR-fail / miscompile before the fix and pass after.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

// `Pair<A,B>` with a Swap (returns Pair<B,A>) and BoxB/BoxA (return Box<B>/
// Box<A>) — the shared fixture for the second-type-parameter scenarios.
// `Swap`/`BoxA`/`BoxB` return `owned` so the caller's `using var` discharges
// the freshly-constructed object (§28.10: a method yielding ownership of a new
// heap object declares `owned`; the bug under test is the LAYOUT/substitution
// of that returned type, independent of the ownership modifier).
const char* kPair =
    "module M; "
    "class Box<T> "
    "  public _val: T; "
    "  public constructor(v: T) _val := v; end constructor; "
    "end Box; "
    "class Pair<A,B> "
    "  public _a: A; "
    "  public _b: B; "
    "  public constructor(a: A, b: B) _a := a; _b := b; end constructor; "
    "  public method Swap(): owned Pair<B,A> return new Pair<B,A>(_b, _a); end Swap; "
    "  public method BoxA(): owned Box<A> return new Box<A>(_a); end BoxA; "
    "  public method BoxB(): owned Box<B> return new Box<B>(_b); end BoxB; "
    "end Pair; ";

int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}

bezel::test::JitResult jit(const std::string& prog) { return runJit(prog); }

}  // namespace

// ── §14.1 / §22.4: Swap() returns Pair<B,A> (CRITICAL — was IR-verify fail) ──

// PRIMARY REPRO (Symptom A): `using var s := p.Swap();` then `return s._b;`.
// `s` is `Pair<string,int>`, so `s._b` is the int `7` carried from `p._a`.
// Before fix: `s` laid out as the receiver type `Pair<int,string>` → `s._b`
// read as `ptr` while main returns i32 → `ret ptr` in an i32 function →
// IR-verify failure (the module never built / never ran).
BEZEL_TEST(generic_second_typeparam_597, swap_second_field_int_14_1_22_4) {
    std::string prog = std::string(kPair) +
        "class C "
        "  public static method Main(): int "
        "    using var p := new Pair<int,string>(7, \"x\"); "
        "    using var s := p.Swap(); "
        "    return s._b; "
        "  end Main; "
        "end C;";
    auto r = jit(prog);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// After Swap, `s._a` is the string carried from `p._b` ("hello" → Length 5).
BEZEL_TEST(generic_second_typeparam_597, swap_first_field_string_14_1_22_4) {
    BEZEL_EXPECT_EQ(run(std::string(kPair) +
        "class C "
        "  public static method Main(): int "
        "    using var p := new Pair<int,string>(7, \"hello\"); "
        "    using var s := p.Swap(); "
        "    return s._a.Length; "
        "  end Main; "
        "end C;"), 5);
}

// ── §14.1 / §22.4: BoxB() returns Box<B> (HIGH — was zero-init) ──────────────

// PRIMARY REPRO (Symptom B): `p.BoxB()._val.Length`.
// `BoxB(): Box<B>` returns `Box<string>`; `._val.Length` is 5.
// Before fix: the call was typed `Box<B>` (unsubstituted) → codegen defaulted
// B to i32 → result laid out as `Box<int>` → `._val` read the string pointer's
// low 32 bits as an int → `.Length` on garbage returned 0.
BEZEL_TEST(generic_second_typeparam_597, boxb_second_param_string_length_14_1_22_4) {
    std::string prog = std::string(kPair) +
        "class C "
        "  public static method Main(): int "
        "    using var p := new Pair<int,string>(42, \"hello\"); "
        "    using var b := p.BoxB(); "
        "    return b._val.Length; "
        "  end Main; "
        "end C;";
    auto r = jit(prog);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// BoxA() returns Box<A> (the FIRST param) — worked pre-fix by luck (A=int and
// the unresolved-param fallback are both i32); must keep working after the fix.
BEZEL_TEST(generic_second_typeparam_597, boxa_first_param_int_14_1_22_4) {
    BEZEL_EXPECT_EQ(run(std::string(kPair) +
        "class C "
        "  public static method Main(): int "
        "    using var p := new Pair<int,string>(42, \"hello\"); "
        "    using var b := p.BoxA(); "
        "    return b._val; "
        "  end Main; "
        "end C;"), 42);
}

// BoxB through an inferred `using var` local (not just inline) — exercises the
// result-local layout recording path (Symptom A's mechanism, Box edition).
BEZEL_TEST(generic_second_typeparam_597, boxb_via_using_var_14_1_22_4) {
    BEZEL_EXPECT_EQ(run(std::string(kPair) +
        "class C "
        "  public static method Main(): int "
        "    using var p := new Pair<int,string>(9, \"world\"); "
        "    using var b := p.BoxB(); "
        "    return b._val.Length; "
        "  end Main; "
        "end C;"), 5);
}

// ── §14.1: three type parameters — return a type built from the THIRD ────────

// A class with THREE params returning `Box<Cc>` (the third). Confirms the fix
// matches the param by index/name, not by a fixed 0/1 assumption.
BEZEL_TEST(generic_second_typeparam_597, triple_third_param_return_14_1) {
    const char* kTriple =
        "module M; "
        "class Box<T> "
        "  public _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "end Box; "
        "class Triple<A,B,Cc> "
        "  public _a: A; "
        "  public _b: B; "
        "  public _c: Cc; "
        "  public constructor(a: A, b: B, c: Cc) "
        "    _a := a; _b := b; _c := c; end constructor; "
        "  public method BoxFirst(): owned Box<A> return new Box<A>(_a); end BoxFirst; "
        "  public method BoxThird(): owned Box<Cc> return new Box<Cc>(_c); end BoxThird; "
        "end Triple; ";
    BEZEL_EXPECT_EQ(run(std::string(kTriple) +
        "class C "
        "  public static method Main(): int "
        "    using var t := new Triple<int,bool,string>(11, true, \"hi!!\"); "
        "    using var b3 := t.BoxThird(); "
        "    using var b1 := t.BoxFirst(); "
        "    return b3._val.Length + b1._val; "  // 4 + 11
        "  end Main; "
        "end C;"), 15);
}

// ── §14.1: mixed value / reference / record type arguments ───────────────────

// A = record (Pt), B = string. Swap()→Pair<string,Pt>; BoxB()→Box<string>.
// Exercises the constructor-argument lowering under the enclosing substitution
// for a RECORD-typed field carried through the swap (segfaulted pre-fix).
BEZEL_TEST(generic_second_typeparam_597, swap_mixed_record_string_args_14_1) {
    const char* kMixed =
        "module M; "
        "record Pt(X: int, Y: int); "
        "class Box<T> "
        "  public _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "end Box; "
        "class Pair<A,B> "
        "  public _a: A; "
        "  public _b: B; "
        "  public constructor(a: A, b: B) _a := a; _b := b; end constructor; "
        "  public method Swap(): owned Pair<B,A> return new Pair<B,A>(_b, _a); end Swap; "
        "  public method BoxB(): owned Box<B> return new Box<B>(_b); end BoxB; "
        "end Pair; ";
    BEZEL_EXPECT_EQ(run(std::string(kMixed) +
        "class C "
        "  public static method Main(): int "
        "    using var p := new Pair<Pt,string>(new Pt(3, 4), \"abcde\"); "
        "    using var s := p.Swap(); "       // Pair<string,Pt>
        "    using var bb := p.BoxB(); "      // Box<string>
        "    return s._a.Length + s._b.X + s._b.Y + bb._val.Length; "  // 5+3+4+5
        "  end Main; "
        "end C;"), 17);
}

// ── §14.1 / §9: a RECORD built from the second class param (was segfault) ─────

// `RecB(): RBox<B>` returns a positional RECORD value built from the second
// param. Pre-fix the record's construction argument `_b` was lowered under the
// swapped substitution and the result laid out wrong → segfault.
BEZEL_TEST(generic_second_typeparam_597, record_return_second_param_14_1_9) {
    const char* kRec =
        "module M; "
        "record RBox<T>(Val: T); "
        "class Pair<A,B> "
        "  public _a: A; "
        "  public _b: B; "
        "  public constructor(a: A, b: B) _a := a; _b := b; end constructor; "
        "  public method RecB(): RBox<B> return new RBox<B>(_b); end RecB; "
        "end Pair; ";
    BEZEL_EXPECT_EQ(run(std::string(kRec) +
        "class C "
        "  public static method Main(): int "
        "    using var p := new Pair<int,string>(42, \"world\"); "
        "    var r := p.RecB(); "
        "    return r.Val.Length; "  // "world" → 5
        "  end Main; "
        "end C;"), 5);
}

// ── Regression guards ────────────────────────────────────────────────────────

// Single-type-parameter generic: a method returning `Box<T>` on `Holder<T>`
// must keep working — the fix must not perturb the one-parameter path.
BEZEL_TEST(generic_second_typeparam_597, single_param_box_return_still_works_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box<T> "
        "  public _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "end Box; "
        "class Holder<T> "
        "  public _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public method Wrap(): owned Box<T> return new Box<T>(_v); end Wrap; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    using var h := new Holder<string>(\"hello\"); "
        "    using var b := h.Wrap(); "
        "    return b._val.Length; "  // 5
        "  end Main; "
        "end C;"), 5);
}

// A bare-type-parameter return (`Get(): A` — the FIRST param, no construction)
// must still substitute correctly: `Pair<string,int>.Get()` is a string.
BEZEL_TEST(generic_second_typeparam_597, bare_first_param_return_still_works_14_1) {
    BEZEL_EXPECT_EQ(run(std::string(kPair) +
        "class C "
        "  public static method Main(): int "
        "    using var p := new Pair<string,int>(\"hello\", 99); "
        "    var a := p._a; "
        "    return a.Length; "  // 5
        "  end Main; "
        "end C;"), 5);
}

// Symmetric check: the SECOND field read directly (no method) on a two-param
// class with the SECOND param a value type must read the right width.
BEZEL_TEST(generic_second_typeparam_597, direct_second_field_read_14_1) {
    BEZEL_EXPECT_EQ(run(std::string(kPair) +
        "class C "
        "  public static method Main(): int "
        "    using var p := new Pair<string,int>(\"hi\", 123); "
        "    return p._b; "  // 123
        "  end Main; "
        "end C;"), 123);
}
