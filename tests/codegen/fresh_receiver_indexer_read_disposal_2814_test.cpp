// Regression tests for #2814 — the INDEX half of §28.3's unbound-owned-temporary
// rule. `(new Src())[i]` never disposed the receiver.
//
// WHAT THE DOCUMENT REQUIRES.  Spec §28.3 "Unbound owned temporaries" (v0.240)
// names both spellings in one sentence:
//
//     An owned value may also be produced as an unbound temporary: the result
//     of an `owned`-returning call (or other owned producer) that is not bound
//     to any name, not `move`d, and not returned, but used only by *borrowing*
//     operations within the expression — as the receiver of a further call, as
//     a non-`owned` argument, as a member or index access, or as the scrutinee
//     of a `case` … Such a temporary is disposed at the end of the full
//     expression in which it is produced, after every use of it within that
//     expression has completed.
//
// The MEMBER half is covered: fields and method calls by #1039/#1053, computed
// properties by #2751.  The INDEX half was not — an indexer read is an
// `ast::IndexExpr` lowered by `lowerIndexerGet`, an entirely different function
// with five getter paths of its own (imported, interface-dispatched, virtual,
// standalone block-bodied, and expression-bodied-inlined).  All five leaked.
//
// MEASURED pre-fix, 20-iteration loop with a ctor+1/dtor-1 live counter:
//     SUMMARY: AddressSanitizer: 320 byte(s) leaked in 20 allocation(s)
// and `Src._live == 20` at exit — not one destructor ran.
//
// WHY THIS CANNOT BE #2261's UAF.  `disposeFreshNewReceiverTemp` disposes only
// when the extracted result is provably free of any reference into the receiver
// (a numeric/bool/char/decimal primitive, an enum, or unit); `string`, `Object`,
// class, interface and struct results are excluded because each may alias into
// it.  The `calleeReturnsOwned` widenings (#1859/#2205) that would relax that
// are passed `false` here BY CONSTRUCTION rather than by choice: §A.3's
// `IndexerDecl` grammar carries no `owned` marker — only `StaticReadonlyDecl`
// has one — so an indexer read can never announce a fresh transfer.
//
// The oracle below is a LIVE-OBJECT COUNTER folded into the exit code, not
// "no leak": a missed disposal reads as a nonzero live count, and a DOUBLE
// disposal as a crash or a wrong count.  "No leak" alone cannot see the second
// direction, which is the dangerous one.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int run(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// A class with a computed indexer, an int element, and a live-object counter.
const char* kSrc =
    "module M; "
    "class Src "
    "  public static _live: int := 0; "
    "  private _v: int := 5; "
    "  private _s: string := \"abc\"; "
    "  public constructor() Src._live := Src._live + 1; end constructor; "
    "  destructor Src._live := Src._live - 1; end destructor; "
    "  public property [i: int]: int get => _v + i; end; "
    "  public method Str(i: int): string => _s; "
    "end Src; ";

std::string main_(const std::string& body) {
    return std::string(kSrc) +
           "class C public static method Main(): int " + body +
           " end Main; end C;";
}
} // namespace

// The reported shape. Loop-amplified because a one-iteration Main is not a
// reliable LSan oracle (a leftover register pointer reads as reachable).
// Pre-fix: 20 live objects and 320 bytes leaked. Post-fix: 0.
BEZEL_TEST(fresh_receiver_indexer_2814,
           fresh_receiver_indexer_read_disposed_spec_28_3) {
    BEZEL_EXPECT_EQ(run(main_(
        "  var i := 0; var total := 0; "
        "  while i < 20 do "
        "    var c := (new Src())[1]; "
        "    total := total + c; "
        "    i := i + 1; "
        "  end while; "
        "  if total <> 120 then return 90; end; "
        "  return Src._live;")), 0);
}

// The temporary is reclaimed in expression positions other than a local-decl
// initializer — position was never the discriminant (that was #2751's finding
// for the property half, and it holds here).
BEZEL_TEST(fresh_receiver_indexer_2814,
           reclaimed_in_argument_and_operand_positions_spec_28_3) {
    BEZEL_EXPECT_EQ(run(main_(
        "  var i := 0; var total := 0; "
        "  while i < 20 do "
        "    total := total + (new Src())[2] + (new Src())[3]; "
        "    i := i + 1; "
        "  end while; "
        "  if total <> 300 then return 90; end; "
        "  return Src._live;")), 0);
}

// DOUBLE-FREE DIRECTION — the one "no leak" cannot see. A BOUND receiver is
// disposed exactly once by its own `using var`; the indexer read must NOT add
// a second disposal. Two reads off the same receiver, twenty times.
BEZEL_TEST(fresh_receiver_indexer_2814,
           bound_receiver_disposed_exactly_once_spec_28_3) {
    BEZEL_EXPECT_EQ(run(main_(
        "  var i := 0; "
        "  while i < 20 do "
        "    using var s := new Src(); "
        "    var a := s[1]; var b := s[2]; "
        "    if a + b <> 13 then return 91; end; "
        "    i := i + 1; "
        "  end while; "
        "  return Src._live;")), 0);
}

// ACCEPT-CONTROL / safe direction: a `string` element may alias into the
// receiver, so the temporary must NOT be reclaimed. This leaks by design —
// asserted only for the VALUE, so the test states the boundary rather than
// pretending the whole surface is covered. runAot (not the checked variant),
// because the deliberate leak would redden LSan.
BEZEL_TEST(fresh_receiver_indexer_2814,
           string_element_receiver_not_reclaimed_spec_28_3) {
    auto r = runAot(main_(
        "  var s := (new Src()).Str(0); "
        "  if s.Length <> 3 then return 90; end; "
        "  return 0;"));
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
}

// ACCEPT-CONTROL: an ordinary indexer read on a plain (non-temporary) local is
// untouched — value correct, receiver still owned by the local.
BEZEL_TEST(fresh_receiver_indexer_2814,
           plain_local_receiver_unchanged_spec_23_4) {
    BEZEL_EXPECT_EQ(run(main_(
        "  using var s := new Src(); "
        "  var v := s[7]; "
        "  if v <> 12 then return 90; end; "
        "  return Src._live - 1;")), 0);
}
