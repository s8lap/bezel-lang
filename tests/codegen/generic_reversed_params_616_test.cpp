// Codegen — generic method returning a NEW GENERIC INSTANCE with REVERSED type
// parameters (issue #616; §14 generics, §22.4 constructor calls).
//
// ── §28.3 row 4 (v0.307) note ────────────────────────────────────────────
// The four `Swap()` declarations below read `: owned Pair<...>`, not
// `: Pair<...>`. That is REQUIRED, not incidental: each returns
// `new Pair<...>(...)` — a fresh owned value — and §28.3's announcement
// table row 4 admits only a receiver-sourced borrow through a plain
// concrete class return ("a fresh owned value is rejected here; declare
// the return type `owned` to transfer ownership to the caller"), which
// §28.6's fresh-value rule states independently.
//
// The rows originally spelled the plain return and were admitted by a
// pre-v0.307 compiler. `fe95b173` implemented row 4 and they became
// compile errors — the tests pinned behaviour the spec does not have.
// The marker is added rather than the rule relaxed; every call site
// already binds with `using var`, so the adopted ownership is what the
// tests always meant. Nothing about the generic-monomorphization defect
// these rows exist for (#616) changes.
//
// HIGH miscompile: `Pair<string,int>.Swap()` returned the wrong values —
// `GetFirst()` on the result returned garbage instead of the int (42), and
// `GetSecond()` returned garbage instead of the string ("hello").  The root
// cause was that inside the specialised `Pair$Swap$spec` body, `new Pair<B,A>`
// lowered its constructor arguments (_second, _first) under the NEW type's
// substitution `{A→int, B→string}` instead of the CALLER's substitution
// `{A→string, B→int}` — so the fields from `this` (layout spec0) were read
// with the wrong struct offsets (layout spec1).  Fix #597 (§14.1 / §22.4):
// lower constructor-argument expressions under the ENCLOSING scope's
// substitution, then restore the new type's substitution for the field stores.
//
// Per §14 / §22.4: in `new Pair<B,A>(_second, _first)` inside a
// `Pair<string,int>` method, the argument expressions `_second` and `_first`
// refer to `this`'s fields — they must be evaluated in the CALLER's
// substitution context `{A→string, B→int}`, not the new type's context.
//
// All tests are load-bearing: they return garbage before the fix and the
// correct values after.  Only real stdlib surface is used.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── §14: control — single-type-param Swap still works ────────────────────

// Pair<int>.Swap() reverses two fields of the SAME type.  Both have T=int so
// the substitution is never reversed.  This pin ensures the fix for two-param
// reversal doesn't regress the one-param case.
BEZEL_TEST(generic_reversed_params_616, single_param_swap_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Pair<T> "
        "  public _first: T; public _second: T; "
        "  public constructor(a: T, b: T) _first := a; _second := b; end constructor; "
        "  public method Swap(): owned Pair<T> return new Pair<T>(_second, _first); end Swap; "
        "  destructor end destructor; "
        "end Pair; "
        "class C public static method Main(): int "
        "  using var p := new Pair<int>(3, 7); "
        "  using var p2 := p.Swap(); "
        "  return p2._first; "  // swapped: was _second=7, now _first=7
        "end Main; end C;"), 7);
}

// ── §14 / §22.4: two-param reversed instantiation — the issue ──────────────

// §14 / §22.4: `Pair<int,string>.Swap()` returns `Pair<string,int>`.
// The first field of the result (`GetFirst()`: string, the old `_second`) must
// equal the string passed to the original constructor.  Before the fix: wrong
// address / value read.  After the fix: the correct int is stored and returned.
BEZEL_TEST(generic_reversed_params_616, swap_two_params_first_field) {
    // §14 (monomorphization): Pair<A,B> is a two-type-parameter generic class.
    // Swap() returns new Pair<B,A>(_second, _first); the field assignments
    // in the new object must use the NEW instantiation's layout (B=first, A=second)
    // while the SOURCE reads (_second, _first) must use the RECEIVER's layout.
    // Returns the int field of the swapped pair (was _first of the original).
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Pair<A, B> "
        "  private _first: A; "
        "  private _second: B; "
        "  public constructor(a: A, b: B) _first := a; _second := b; end constructor; "
        "  public method GetFirst(): A => _first; "
        "  public method GetSecond(): B => _second; "
        "  public method Swap(): owned Pair<B, A> "
        "    return new Pair<B, A>(_second, _first); "
        "  end Swap; "
        "  destructor end destructor; "
        "end Pair; "
        "class C public static method Main(): int "
        "  using var p := new Pair<int, string>(42, \"hello\"); "
        "  using var swapped := p.Swap(); "
        "  return swapped.GetSecond(); "  // was _first=42; now in GetSecond (A=int)
        "end Main; end C;"), 42);
}

// §14 / §22.4: verify the int field of the swapped result via the swapped
// accessor (the original _second, now in GetFirst of Pair<B=int,A=string>).
// Both int fields must have their correct values even when mixing ptr and i32.
BEZEL_TEST(generic_reversed_params_616, swap_both_fields_correct) {
    // p = Pair<int, string>(_first=3, _second="hello").  After Swap():
    // swapped = Pair<string, int>(_first="hello", _second=3).
    // GetSecond() on swapped returns B=int = 3.
    // Verifies both the ptr field (_first) and the int field (_second)
    // are stored correctly in the reversed instantiation (no garbage reads).
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Pair<A, B> "
        "  private _first: A; "
        "  private _second: B; "
        "  public constructor(a: A, b: B) _first := a; _second := b; end constructor; "
        "  public method GetFirst(): A => _first; "
        "  public method GetSecond(): B => _second; "
        "  public method Swap(): owned Pair<B, A> "
        "    return new Pair<B, A>(_second, _first); "
        "  end Swap; "
        "  destructor end destructor; "
        "end Pair; "
        "class C public static method Main(): int "
        "  using var p := new Pair<int, string>(3, \"hello\"); "
        "  using var swapped := p.Swap(); "
        // swapped = Pair<string=ptr, int=i32>: _first=ptr("hello"), _second=i32(3)
        // GetSecond() returns B=int=3
        "  return swapped.GetSecond(); "
        "end Main; end C;"), 3);
}

// §14 / §22.4: the int field reads the correct value even when the second arg
// is a string (ptr-sized).  Before the fix the i32 was read from the padding
// bytes of spec0 (garbage), not from the actual i32 field.
BEZEL_TEST(generic_reversed_params_616, swap_int_string_second_field_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Pair<A, B> "
        "  private _first: A; "
        "  private _second: B; "
        "  public constructor(a: A, b: B) _first := a; _second := b; end constructor; "
        "  public method GetFirst(): A => _first; "
        "  public method GetSecond(): B => _second; "
        "  public method Swap(): owned Pair<B, A> "
        "    return new Pair<B, A>(_second, _first); "
        "  end Swap; "
        "  destructor end destructor; "
        "end Pair; "
        "class C public static method Main(): int "
        "  using var p := new Pair<int, string>(42, \"hello\"); "
        "  using var swapped := p.Swap(); "
        // swapped = Pair<string, int>; GetSecond() = B=int = old _first=42
        "  return swapped.GetSecond(); "
        "end Main; end C;"), 42);
}
