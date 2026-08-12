// Spec-derived ACCEPT controls for issue #2183 — a generic CHOICE classifies
// per-instantiation (§35.4 / §35.11): borrow-bearing at a borrow-carrying type
// argument, value-like at a value/runtime-managed one. The REJECT side (span/
// class-substituted async parameter and result) is pinned by the lit tests
// (tests/lit/ownership/generic_choice_async_borrow_2183__*_rejected.bzl). This
// file pins the ACCEPT side — the over-reject failure mode: the instantiation-
// aware borrow leg must reclassify a VALUE-substituted generic choice to value-
// like so it stays a legal `async` parameter AND a hashable value. A naive "any
// generic choice is borrow-bearing" reading would reject both. Runs under the
// AOT ASan/LSan oracle.
//
// per §35.4 (v0.226), §35.7, §30.1, §28.3

#include <string>

#include "support/aot_support.hpp"  // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

// §35.4.3 / §30.1: `Opt<int>` is value-like at this instantiation, so it is a
// legal `async` parameter and the method runs through a real suspension. The
// captured choice is an ordinary copyable value (no frame-escaping borrow).
// Some(41) -> 41 + 1 == 42.
BEZEL_TEST(generic_choice_async_borrow_2183, value_arg_async_param_runs_accepted) {
    std::string src =
        "module M; choice Opt<T> Some(v: T), None; end Opt; "
        "class C "
        "  public static async method RunAsync(x: Opt<int>): Task<int> "
        "    await Task.Delay(0); "
        "    var acc: int := 0; "
        "    case x of Some(var v) => do acc := v; end, None => do end end; "
        "    return acc + 1; "
        "  end RunAsync; "
        "  public static method Main(): int "
        "    var r: int := 0; "
        "    do "
        "      using var t := C.RunAsync(Opt<int>.Some(41)); "
        "      t.Wait(); "
        "      r := t.Result; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// §35.7 / §35.4.3: a value-like `Opt<int>` is auto-hashable and
// hashable-admissible, so it is a valid Dictionary key (keys hash+compare by
// tag + active payload) — exactly the stdlib `Result<int, string>` precedent,
// now available to a user generic choice at a value instantiation. Two distinct
// keys coexist and look up. 100 + 7 == 107.
BEZEL_TEST(generic_choice_async_borrow_2183, value_arg_dictionary_key_accepted) {
    std::string src =
        "module M; choice Opt<T> Some(v: T), None; end Opt; "
        "class C "
        "  public static method Main(): int "
        "    var d: Dictionary<Opt<int>, int> := "
        "      new Dictionary<Opt<int>, int>(); "
        "    d[Opt<int>.Some(5)] := 100; "
        "    d[Opt<int>.None] := 7; "
        "    var acc: int := 0; "
        "    if d.ContainsKey(Opt<int>.Some(5)) then "
        "      acc := acc + d[Opt<int>.Some(5)]; end; "
        "    if d.ContainsKey(Opt<int>.None) then "
        "      acc := acc + d[Opt<int>.None]; end; "
        "    return acc; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 107);
}
