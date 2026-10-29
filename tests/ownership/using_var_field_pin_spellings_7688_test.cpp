// Every SPELLING of a field read is the same `using var` borrow-pin (issue
// #7688; spec §28.10 *`using var` initialized from a borrow*, §17.8, §7.5,
// §19.3 / §35.5, v0.350).
//
// THE GOVERNING TEXT.
//
//   §28.10 (v0.350), *`using var` initialized from a borrow*: "When the
//   right-hand side of a `using var` declaration is a borrow rather than an
//   owned value (… an indexer access, a field read, or an unmarked read of a
//   **borrow-state** named binding …), the `using var` does **not** assume a
//   disposal obligation …: at scope end, no `Dispose()` is called".
//
//   §17.8: `x!` is classified by its position exactly as the bare `x` is —
//   so `h.G!` is a field read.
//
//   §7.5 / §28.3: `h?.F` reads the same storage as `h.F`; its `null` path
//   "has no value to own" and is compatible with either side.
//
//   §28.6: a property getter is a borrow-producing position (it "may not
//   return a freshly-produced owned value"), and no move-only value type is
//   mint-announcing — so `h.PF` over a move-only field is a borrow too.
//
//   §28.10 (meet paragraph): "A meet with any other borrow arm — … a field
//   … — … the `using var` is a borrow-pin of the union of sources"; `null`
//   and `throw` arms absorb (K7.5), and a nested meet is examined by its own
//   arms.
//
//   §19.3 / §35.5: a BORROWING-match payload binding is "a borrow whose
//   source is the scrutinee's source" — one of the borrow-state bindings a
//   `using var` only pins.
//
// THE BUG (#7688). Staging 106e3167 (narrow #7480) taught codegen's move-only
// registrations to skip a pinned bare field read `h.F`, but the ownership
// pass set the pin flag only for the bare `.` / implicit-`this` spelling and
// codegen keyed on that spelling too. Through `h?.F`, `h.G!`, a property, an
// if / `??` / `case` meet, and a borrowing-match `case` binding, `x` still
// disposed the payload at its scope end and the holder (or scrutinee)
// disposed it again: a heap-use-after-free under ASan.
//
// THE ORACLE. Every row runs under ASan/LSan (`runAotChecked`) and loops the
// shape 100 times. A static counter in the destructor counts disposals; each
// iteration checks that no disposal happened before the holder died (a pin
// that disposed bumps the counter early), and the final count shows each
// object disposed exactly once (a doubled disposal is also an ASan error).
//
// per §28.10, §17.8, §7.5, §28.6, §19.3, §35.5, v0.350

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// `Res.Count` counts disposals. `H` holds two owned `Res`: one in the owning
// choice field `F` and one in the nullable owning choice field `G`; `A` is an
// owning inline array whose slot 0 holds a third.
const char* kDecls =
    "public class Res "
    "  public static Count: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res.Count := Res.Count + 1; end destructor; "
    "end Res; "
    "public choice OC Text(owned r: Res), Empty; end OC; "
    "public class H "
    "  public owned F: OC := OC.Text(new Res()); "
    "  public owned G: OC? := OC.Text(new Res()); "
    "  public A: [2 owned]Res? := [new Res(), null]; "
    "  public property PF: OC => F; "
    "  public property PG: OC? => G; "
    "  public property PA: [2 owned]Res? => A; "
    "  public property BF: OC get => F; end BF; "
    "  public constructor() end constructor; "
    "end H; "
    "public choice Opt Some(owned r: Res), None; end Opt; ";

// `Pin(hn, h2, c, n)` runs `pinBody` against two live holders; after each of
// the 100 iterations both holders are gone, so the count must be 6 * (i + 1),
// and right after `Pin` returns (holders alive) it must still be 6 * i.
std::string pinProg(const std::string& pinBody) {
    return std::string("module M; ") + kDecls +
           "public class Program "
           "  public static method Pin(hn: H?, h2: H, c: bool, n: int) " +
           pinBody +
           "  end Pin; "
           "  public static method Main(): int "
           "    var early := 0; "
           "    for var i in 0..<100 do "
           "      do var h := new H(); var h2 := new H(); "
           "        Pin(h, h2, (i mod 2) = 0, i mod 3); "
           "        if Res.Count <> 6 * i then early := early + 1; end if; "
           "      end; "
           "      if Res.Count <> 6 * (i + 1) then early := early + 1; end if; "
           "    end; "
           "    Console.WriteLine($\"{early} {Res.Count}\"); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

void expectRunOutput(const JitResult& r, const std::string& expected) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// --- the `?.` spelling (§7.5) -------------------------------------------------

BEZEL_TEST(ownership_using_var_field_pin_spellings_7688,
           spec_28_10_using_var_over_a_null_conditional_move_only_field_is_a_pin) {
    // The issue's m05 shape: `hn?.F` over an owning choice, `hn?.G` over a
    // nullable one.
    expectRunOutput(runAotChecked(pinProg(
                        "do using var x := hn?.F; end; "
                        "do using var y := hn?.G; end; ")),
                    "0 600\n");
}

// --- the `!` spelling (§17.8) -------------------------------------------------

BEZEL_TEST(ownership_using_var_field_pin_spellings_7688,
           spec_17_8_using_var_over_a_bang_move_only_field_is_a_pin) {
    // The issue's s01 shape `h.G!`, and the `?.`-then-`!` composition.
    expectRunOutput(runAotChecked(pinProg(
                        "do using var x := h2.G!; end; "
                        "do using var y := hn?.F!; end; "
                        "do using var z := hn!.G!; end; ")),
                    "0 600\n");
}

// --- a property read (§28.6) --------------------------------------------------

BEZEL_TEST(ownership_using_var_field_pin_spellings_7688,
           spec_28_10_using_var_over_a_move_only_property_read_is_a_pin) {
    // The issue's s03 shape: expression-bodied and block getters, the
    // inline-array property, and the `?.` / `!` spellings of a property.
    expectRunOutput(runAotChecked(pinProg(
                        "do using var x := h2.PF; end; "
                        "do using var y := h2.PA; end; "
                        "do using var z := h2.BF; end; "
                        "do using var u := hn?.PF; end; "
                        "do using var v := h2.PG!; end; "
                        "do using var w := hn?.PG!; end; ")),
                    "0 600\n");
}

// --- a meet of field reads (§28.10, K7.5) ------------------------------------

BEZEL_TEST(ownership_using_var_field_pin_spellings_7688,
           spec_28_10_using_var_over_an_if_meet_of_move_only_fields_is_a_pin) {
    // The issue's s07 shape, over both move-only kinds, with a property arm
    // and a `null` arm (absorbing, K7.5).
    expectRunOutput(runAotChecked(pinProg(
                        "do using var x := (if c then hn!.F else h2.F); end; "
                        "do using var y := (if c then hn!.A else h2.PA); end; "
                        "do using var z := (if c then h2.G else null); end; ")),
                    "0 600\n");
}

BEZEL_TEST(ownership_using_var_field_pin_spellings_7688,
           spec_28_10_using_var_over_a_coalesce_and_case_meet_of_fields_is_a_pin) {
    // `??` over two field reads, an expression-`case` whose arms are field and
    // property reads, and a nested meet examined by its own arms.
    expectRunOutput(runAotChecked(pinProg(
                        "do using var x := hn?.G ?? h2.G; end; "
                        "do using var y := (case n of 0 => hn!.F, "
                        "  1 => (if c then h2.F else hn!.PF), _ => h2.BF); end; ")),
                    "0 600\n");
}

// --- a borrowing-match `case` binding (§19.3, §35.5) --------------------------

BEZEL_TEST(ownership_using_var_field_pin_spellings_7688,
           spec_35_5_using_var_over_a_borrowing_match_binding_is_a_pin) {
    // The issue's o09 shape: `r` borrows the scrutinee's payload; the pin
    // disposes nothing, and `o`'s own teardown disposes the `Res` once.
    expectRunOutput(
        runAotChecked(std::string("module M; ") + kDecls +
                      "public class Program "
                      "  public static method Main(): int "
                      "    var early := 0; "
                      "    for var i in 0..<100 do "
                      "      do var o := Opt.Some(new Res()); "
                      "        case o of "
                      "          Some(var r) => do using var x := r; end, "
                      "          None => do end "
                      "        end; "
                      "        if Res.Count <> i then early := early + 1; end if; "
                      "      end; "
                      "      if Res.Count <> i + 1 then early := early + 1; end if; "
                      "    end; "
                      "    Console.WriteLine($\"{early} {Res.Count}\"); "
                      "    return 0; "
                      "  end Main; "
                      "end Program;"),
        "0 100\n");
}

// --- controls: an owned move-only right-hand side still disposes ------------

BEZEL_TEST(ownership_using_var_field_pin_spellings_7688,
           spec_28_10_control_using_var_over_a_move_only_local_or_param_still_adopts) {
    // "an unmarked read of a **move-only value** binding moves instead and is
    // an owned right-hand side": a local, a plain parameter and an `owned`
    // parameter keep their disposal (the widened shape test keys on a FIELD
    // or PROPERTY, never a local or parameter symbol).
    expectRunOutput(
        runAotChecked(std::string("module M; ") + kDecls +
                      "public class Program "
                      "  public static method T(p: OC) using var x := p; end T; "
                      "  public static method U(owned p: OC) using var x := p; end U; "
                      "  public static method Main(): int "
                      "    for var i in 0..<100 do "
                      "      do var o := OC.Text(new Res()); using var x := o; end; "
                      "      var o1 := OC.Text(new Res()); T(o1); "
                      "      var o2 := OC.Text(new Res()); U(move o2); "
                      "    end; "
                      "    Console.WriteLine($\"{Res.Count}\"); "
                      "    return 0; "
                      "  end Main; "
                      "end Program;"),
        "300\n");
}

BEZEL_TEST(ownership_using_var_field_pin_spellings_7688,
           spec_28_10_control_using_var_over_a_fresh_owning_choice_meet_disposes) {
    // An owned meet (both arms fresh producers) is an owned right-hand side
    // and is disposed once at scope end.
    expectRunOutput(
        runAotChecked(std::string("module M; ") + kDecls +
                      "public class Program "
                      "  public static method Main(): int "
                      "    var early := 0; "
                      "    for var i in 0..<100 do "
                      "      do using var x := (if (i mod 2) = 0 "
                      "        then OC.Text(new Res()) else OC.Text(new Res())); end; "
                      "      if Res.Count <> i + 1 then early := early + 1; end if; "
                      "    end; "
                      "    Console.WriteLine($\"{early} {Res.Count}\"); "
                      "    return 0; "
                      "  end Main; "
                      "end Program;"),
        "0 100\n");
}
