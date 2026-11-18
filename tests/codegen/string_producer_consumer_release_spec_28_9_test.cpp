// Codegen — a +1 `string` handed to a consumer that only READS it.
//
// §28.9 (v0.357) exempts `string` from manual disposal: "the type's lifetime
// is managed by the runtime without user involvement."
//
// A producer — a concatenation, a built-in or user method that returns a
// fresh string — hands its consumer one reference. Each row below is a
// consumer that inspects the string and keeps nothing (a null comparison, a
// discarded statement, `_ :=`, a formatted interpolation hole), so that
// consumer gives the reference back.
// Every row loops 200x under `runAotChecked`, whose leak check is the oracle:
// before its fix each row leaked one string per producer per iteration.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

// #8125 (§8.4 / §28.9): a +1 string compared against `null` is read for its
// null-ness only and must be released there. The issue's own two spellings
// (`s?.Trim()`, `s!.ToUpper()`) and a user method's fresh `string?`, on
// either side of `=` / `<>`. 200x.
BEZEL_TEST(codegen_str_consumer_release, string_producer_null_comparison_is_released_8125_spec_8_4) {
    auto r = runAotChecked(
        "module M; "
        "class H "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public method Mint(): string return \"KM\" + _n.ToString(); end Mint; "
        "  public method Long(): string return \"long-producer-string-no-\" + _n.ToString(); end Long; "
        "  public method MintQ(): string? return \"KQ\" + _n.ToString(); end MintQ; "
        "  public property P: string get => \"KP\" + _n.ToString(); end P; "
        "end H; "
        "class C public static method Main(): int "
        "  var i := 0; var tot := 0; "
        "  var s: string? := \"  abcde  \"; "
        "  var h := new H(7); "
        "  while i < 200 do "
        "    i := i + 1; "
        "    if (s?.Trim()) <> null then tot := tot + 1; end; "
        "    if (s!.ToUpper()) <> null then tot := tot + 1; end; "
        "    if h.MintQ() <> null then tot := tot + 1; end; "
        "    if h.MintQ() = null then tot := tot + 100; end; "
        "    if null <> h.MintQ() then tot := tot + 1; end; "
        "  end while; "
        "  if tot = 200 * 4 then return 1; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 1);
}
