// Ownership -- a local initialised from (or assigned) `E!` over an `owned`
// mint is OWNED exactly as the bare `E` is (issue #7678; spec v0.350).
//
// Spec, v0.350:
//   sec 17.8: "if `x` has ownership state owned ... `x!` is owned; if `x` is
//   borrowed, `x!` is borrowed with the same source." and "Over a named owned
//   binding, `x!` is therefore classified by its position exactly as the bare
//   read `x` is." The `!` is "purely a type-system assertion": it mints and
//   consumes nothing.
//   sec 28.3 (producers): a call to an `owned`-returning method, and a call
//   whose declared return is mint-announcing (announcement table rows 1-3), is
//   OWNED at the call site.
//   sec 28.8: an `owned` sink takes an owned value; a borrow is rejected.
//
// The ownership pass's producerState did not look through a postfix `!`, so
// `var e := h.Mk()!;` bound a BORROW: `s.Take(move e)` was refused ("a borrow
// cannot be passed where an `owned` value is required"), and `e := h.Mk()!;`
// silently leaked the displaced value that the bare spelling rejects ("owned
// value in `e` would be dropped"). The mirror must hold: `!` over a BORROWED
// operand (a named owned local read, a field, a plain parameter) stays a
// borrow, so `move b` of it is still refused.
//
// Every runtime row loops 50x under generated-code ASan/LSan (runAotChecked)
// and pins exact stdout; each `!` row has a no-bang control.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// stdout (trailing newline stripped) of a clean, ASan/LSan-checked run, or a
// marker naming the failure (compile error, non-zero exit incl. a leak report).
std::string out(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) return "<did not run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    std::string s = r.stdoutText;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// `H.Mk()` / `H.MkW()` mint an `owned` frame / class instance; `Sink` stores a
// frame in an `owned` field (disposed with the Sink) or consumes a `W`.
const char* kPrelude = R"BZL(
module M;
public class W
  public static ds: int := 0;
  public v: int;
  public constructor(v: int) this.v := v; end constructor;
  destructor W.ds := W.ds + 1; end destructor;
end W;
public class H
  public static fins: int := 0;
  public owned Fld: IEnumerable<int>?;
  public constructor() end constructor;
  public method GenS(): owned IEnumerable<int>
    try yield return 1; yield return 2;
    finally H.fins := H.fins + 1; end;
  end GenS;
  public method Mk(): owned IEnumerable<int>?
    return this.GenS();
  end Mk;
  public method MkW(): owned W? return new W(3); end MkW;
end H;
public class Sink
  public owned Held: IEnumerable<int>?;
  public constructor() end constructor;
  public method Take(owned e: IEnumerable<int>?) Held := e; end Take;
  public method SumHeld(): int
    var t := 0;
    for var x in Held! do t := t + x; end;
    return t;
  end SumHeld;
  public static method EatW(owned w: W?) end EatW;
end Sink;
)BZL";

std::string loopProg(const std::string& body) {
    return std::string(kPrelude) +
           "public class C\n"
           "  public static method Main(): int\n"
           "    var sum := 0; var i := 0;\n"
           "    while i < 50 do\n"
           "      using var h := new H();\n"
           "      using var s := new Sink();\n" +
           body +
           "      i := i + 1;\n"
           "    end;\n"
           "    Console.WriteLine(\"sum=\" + sum.ToString() + \" fins=\" + "
           "H.fins.ToString() + \" ds=\" + W.ds.ToString());\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// A reject program: `body` sits in a static method taking `h` and `s`.
std::string rejectProg(const std::string& extraMembers,
                       const std::string& body) {
    return std::string(kPrelude) +
           "public class C\n" + extraMembers +
           "  public static method Main(): int\n"
           "    using var h := new H();\n"
           "    using var s := new Sink();\n"
           "    var sum := 0;\n" +
           body +
           "    return sum;\n"
           "  end Main;\n"
           "end C;\n";
}

const char* kBorrowNeedle =
    "a borrow cannot be passed where an `owned` value is required";
const char* kDropNeedle = "owned value in `e` would be dropped here";

void expectRejected(const std::string& src, const char* needle) {
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
}

}  // namespace

// --- `move e` of a `!`-bound owned mint (spec 17.8 / 28.3 / 28.8) -------------

BEZEL_TEST(ownership_bang_owned_mint_local_7678, var_bang_move_into_owned_sink_50x) {
    BEZEL_EXPECT_EQ(
        out(loopProg("      var e := h.Mk()!; s.Take(move e); "
                     "sum := sum + s.SumHeld();\n")),
        "sum=150 fins=50 ds=0");
}

BEZEL_TEST(ownership_bang_owned_mint_local_7678, control_var_bare_move_into_owned_sink_50x) {
    BEZEL_EXPECT_EQ(
        out(loopProg("      var e := h.Mk(); s.Take(move e); "
                     "sum := sum + s.SumHeld();\n")),
        "sum=150 fins=50 ds=0");
}

// A nested `!!` peels every layer.
BEZEL_TEST(ownership_bang_owned_mint_local_7678, var_double_bang_move_into_owned_sink_50x) {
    BEZEL_EXPECT_EQ(
        out(loopProg("      var e := h.Mk()!!; s.Take(move e); "
                     "sum := sum + s.SumHeld();\n")),
        "sum=150 fins=50 ds=0");
}

// A class-typed `owned` mint under `!`.
BEZEL_TEST(ownership_bang_owned_mint_local_7678, var_bang_class_mint_move_50x) {
    BEZEL_EXPECT_EQ(
        out(loopProg("      var w := h.MkW()!; Sink.EatW(move w);\n")),
        "sum=0 fins=0 ds=50");
}

BEZEL_TEST(ownership_bang_owned_mint_local_7678, control_var_bare_class_mint_move_50x) {
    BEZEL_EXPECT_EQ(
        out(loopProg("      var w := h.MkW(); Sink.EatW(move w);\n")),
        "sum=0 fins=0 ds=50");
}

// --- reassignment of a `!`-bound owned local ----------------------------------

// After `move e` the local is moved-from; reassigning it from `h.Mk()!` binds a
// fresh owned value, which the second `move` hands on: nothing leaks.
BEZEL_TEST(ownership_bang_owned_mint_local_7678, reassign_bang_after_move_50x) {
    BEZEL_EXPECT_EQ(
        out(loopProg("      var e := h.Mk()!; s.Take(move e); "
                     "e := h.Mk()!; s.Take(move e); "
                     "sum := sum + s.SumHeld();\n")),
        "sum=150 fins=50 ds=0");
}

BEZEL_TEST(ownership_bang_owned_mint_local_7678, control_reassign_bare_after_move_50x) {
    BEZEL_EXPECT_EQ(
        out(loopProg("      var e := h.Mk(); s.Take(move e); "
                     "e := h.Mk(); s.Take(move e); "
                     "sum := sum + s.SumHeld();\n")),
        "sum=150 fins=50 ds=0");
}

// Reassigning a still-owned local drops the displaced value: the bare spelling
// is an error (spec 28.3 "owned value ... would be dropped"), and `!` must not
// turn it into a silent leak.
BEZEL_TEST(ownership_bang_owned_mint_local_7678, reassign_bang_over_live_owned_rejected) {
    expectRejected(rejectProg("", "    var e := h.Mk()!;\n"
                                  "    e := h.Mk()!;\n"),
                   kDropNeedle);
}

BEZEL_TEST(ownership_bang_owned_mint_local_7678, control_reassign_bare_over_live_owned_rejected) {
    expectRejected(rejectProg("", "    var e := h.Mk();\n"
                                  "    e := h.Mk();\n"),
                   kDropNeedle);
}

// --- the mirror: `!` over a BORROWED operand stays a borrow --------------------

// A named owned local read: `o!` is `o`'s unmarked read, a borrow of `o`.
BEZEL_TEST(ownership_bang_owned_mint_local_7678, mirror_bang_over_named_owned_local_stays_borrow) {
    expectRejected(rejectProg("", "    var o := h.Mk();\n"
                                  "    var b := o!;\n"
                                  "    s.Take(move b);\n"
                                  "    s.Take(move o);\n"),
                   kBorrowNeedle);
}

BEZEL_TEST(ownership_bang_owned_mint_local_7678, control_bare_over_named_owned_local_borrow) {
    expectRejected(rejectProg("", "    var o := h.Mk();\n"
                                  "    var b := o;\n"
                                  "    s.Take(move b);\n"
                                  "    s.Take(move o);\n"),
                   kBorrowNeedle);
}

// A field read.
BEZEL_TEST(ownership_bang_owned_mint_local_7678, mirror_bang_over_field_stays_borrow) {
    expectRejected(rejectProg("", "    var b := h.Fld!;\n"
                                  "    s.Take(move b);\n"),
                   kBorrowNeedle);
}

// A plain parameter.
BEZEL_TEST(ownership_bang_owned_mint_local_7678, mirror_bang_over_plain_parameter_stays_borrow) {
    expectRejected(
        rejectProg("  public static method F(p: IEnumerable<int>?, s: Sink)\n"
                   "    var b := p!;\n"
                   "    s.Take(move b);\n"
                   "  end F;\n",
                   ""),
        kBorrowNeedle);
}

BEZEL_TEST(ownership_bang_owned_mint_local_7678, control_bare_plain_parameter_borrow) {
    expectRejected(
        rejectProg("  public static method F(p: IEnumerable<int>?, s: Sink)\n"
                   "    var b := p;\n"
                   "    s.Take(move b);\n"
                   "  end F;\n",
                   ""),
        kBorrowNeedle);
}
