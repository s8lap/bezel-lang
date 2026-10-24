// fix #7014 / #7025 (follow-up, §27.6 / §28.2 / §28.6, v0.343): the
// method-group RECEIVER spelled as a FIELD ACCESS (`this.d`).
//
// THE DEFECT. §27.6: "the instance is captured at formation — by reference
// for class- or interface-typed receivers". §28.2: "An explicit `Dispose()`
// … performs **logical disposal only** — it sets the flag and runs the
// class's cleanup — and does not reclaim its receiver: the receiver's block
// stays live until the holder's release point". §28.6: for an `owned` FIELD
// the explicit call is "an early release point, not a discharge of the slot's
// obligation … the compiler's single free stays at the holder's release
// point, the containing object's teardown".
//
// The borrow fact that decides whether the explicit `Dispose()` may reclaim
// eagerly lives on the receiver's SYMBOL. The receiver was resolved through
// three predicates that all required an `IdentifierExpr` — so the EXPLICIT
// field spelling (`this.d`, a `MemberAccessExpr`) was never stamped, while
// the BARE spelling (`d`, an `IdentifierExpr`) was. The two spellings denote
// the SAME storage (sema binds the same FieldSymbol to both), so the fact
// must not depend on the spelling.
//
// MEASURED on the tip, on the shape below (the ASan report is the reviewer's
// and the coordinator's, reproduced by hand):
//
//   ==...==ERROR: AddressSanitizer: heap-use-after-free on address 0x...
//   READ of size 8 ... #0 in disposethunk$Dispose
//                   #1 in bz$M.Holder$Go
//   freed by thread T0 here: #1 in bz$M.Holder$Go
//   previously allocated by thread T0 here: #1 in bz$M.Holder$ctor
//
// The FREE was inside `bz$M.Holder$Go` — the method holding the explicit
// dispose — not at the containing object's teardown, which is §28.6's release
// point for an `owned` field.
//
// THE ROWS BELOW. The explicit spelling must behave EXACTLY like the bare
// one: the explicit `Dispose()` performs the logical half only, the group's
// later invocation reads a LIVE block (§28.2), and the single free happens
// at the holder's release point. Each row is pinned with `runAotChecked`, so
// the AOT leg runs the program under AddressSanitizer/LeakSanitizer: the
// pre-fix use-after-free fails the row (the harness reports "sanitizer
// fired") and a leak or a double free fails it too.
//
// The bare-spelling row is the CONTROL: it already behaved this way, and a
// fix that only moved the explicit spelling would leave the two disagreeing.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// The class D with an observable destructor, and the class Holder with the
// `owned` field — the §28.5 shape whose field the group captures.
constexpr char kOwnedFieldHolder[] =
    "module M; "
    "class D "
    "  public constructor() end constructor; "
    "  destructor Console.WriteLine(\"disposed\"); end destructor; "
    "end D; "
    "class Holder "
    "  public owned d: D; "
    "  public constructor() d := new D(); end constructor; "
    "  public method Go(): int ";

}  // namespace

// The EXPLICIT field spelling: the group is formed over `this.d`, the field
// is explicitly disposed, and the group is invoked afterwards.
//
// §28.2: the receiver's block must stay live until the holder's release
// point, so the invocation reads a live block; §28.6: the free belongs to
// the containing object's teardown, not to the explicit call.
//
// PRE-FIX: AddressSanitizer "heap-use-after-free … READ of size 8" in
// `disposethunk$Dispose`, freed inside `bz$M.Holder$Go`.
BEZEL_TEST(method_group_field_receiver_7014,
           spec_28_6_owned_field_group_receiver_stays_live_until_the_holder_release_point) {
    auto r = runAotChecked(
        std::string(kOwnedFieldHolder) +
        "  var f := this.d.Dispose; "
        "  this.d.Dispose(); "
        "  f(); "
        "  Console.WriteLine(\"after\"); "
        "  return 0; "
        "end Go; end Holder; "
        "class C public static method Main(): int "
        "  var h := new Holder(); "
        "  Console.WriteLine(h.Go()); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // The destructor runs EXACTLY once: the explicit `Dispose()` performs the
    // logical disposal (running the cleanup), and the group's later
    // invocation is the already-set flag's no-op (§28.2: "a repeated call is
    // the no-op").
    BEZEL_EXPECT_EQ(r.stdoutText, "disposed\nafter\n0\n");
}

// The CONTROL: the BARE spelling of the same receiver (`d` instead of
// `this.d`). It already behaved this way — the bare spelling resolves to
// the same FieldSymbol — and it must keep behaving this way: the two
// spellings are ONE storage, so the borrow fact, the logical-only dispose
// and the release point are the same for both.
//
// PRE-FIX: this row was already green; it is here so that a fix which moved
// only the explicit spelling (leaving the two disagreeing) would be caught.
BEZEL_TEST(method_group_field_receiver_7014,
           spec_28_6_owned_field_group_receiver_matches_the_bare_field_spelling) {
    auto r = runAotChecked(
        std::string(kOwnedFieldHolder) +
        "  var f := d.Dispose; "
        "  d.Dispose(); "
        "  f(); "
        "  Console.WriteLine(\"after\"); "
        "  return 0; "
        "end Go; end Holder; "
        "class C public static method Main(): int "
        "  var h := new Holder(); "
        "  Console.WriteLine(h.Go()); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "disposed\nafter\n0\n");
}

// The same storage, the group formed AFTER the explicit dispose. §28.2's
// use-after-dispose regime covers this program (the compiler warns — the
// diagnostic is pinned in tests/ownership/), and the RUNTIME obligation is
// that the capture must not be a null pointer and must not be a freed one:
// the group's invocation is the already-set dispose flag's no-op.
//
// PRE-FIX (the explicit spelling): the explicit `Dispose()` reclaimed the
// field's block eagerly, so the group captured a freed block.
BEZEL_TEST(method_group_field_receiver_7014,
           spec_28_2_owned_field_group_formed_after_the_dispose_does_not_crash) {
    auto r = runAotChecked(
        std::string(kOwnedFieldHolder) +
        "  this.d.Dispose(); "
        "  var f := this.d.Dispose; "
        "  f(); "
        "  Console.WriteLine(\"after\"); "
        "  return 0; "
        "end Go; end Holder; "
        "class C public static method Main(): int "
        "  var h := new Holder(); "
        "  Console.WriteLine(h.Go()); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "disposed\nafter\n0\n");
}
