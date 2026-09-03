// Spec-derived regression tests for issues #3743 and #3744 — the two remaining
// §28.3/§28.9 owned-binding escape channels reachable from inside a lambda
// body, both measured as compiles-clean-then-SIGSEGV on the parent (9feef15).
//
// THE RULES, quoted from the spec (v0.286):
//
//   §28.9 "Runtime-managed types": "Capturing an *owned* class-typed local
//   does **not** transfer ownership into the closure: the capture is a borrow
//   whose source is the local's enclosing scope, borrow-state throughout — the
//   closure may not assign to, transfer from, or dispose the captured binding,
//   and may not outlive that scope (§28.3)".
//
//   §28.3 *Borrow lifetime rules*: "A borrow may not be assigned to a local
//   whose scope strictly encloses the borrow's source's scope".
//
//   §28.3 *Borrow lifetime rules*, clause 1: "A borrowed scope-bound *local*
//   never satisfies the rule — it is disposed at the end of its scope, which a
//   field of the object outlives — so a borrowed local may not be assigned to
//   any field."
//
//   §28.3 *Auto-drop (affine)*: an undischarged `owned` parameter "is disposed
//   implicitly at that scope end (for an `owned` parameter, at the end of the
//   method)".
//
//   §28.3 *Captured owned bindings*: "Inside a closure, a captured owned
//   binding — an owned local or `owned` parameter of the enclosing body — is
//   uniformly in **borrow state**: reads borrow".
//
// THE TWO HOLES.
//
// (#3743, the SOURCE-FRAME axis.) fix #3727 sealed the frame
// `collectLocalDepths` walks FIRST and re-asks `ownedLocalBorrowSource` about
// it when the lambda-body-scoped query comes back empty. That walk never
// enters a `LambdaExpr`, so an owned local declared in an ENCLOSING LAMBDA's
// own body is a key of NO map — neither the checked body's re-seeded one nor
// the sealed method frame — and `checkBorrowStoreEscape` returned before any
// comparison. The fix seals a CHAIN: each entry carries the frame that
// declared it beside its block depth within that frame, and the "strictly
// encloses" comparison is lexicographic on the pair.
//
// (#3744, the DESTINATION-CHANNEL axis.) #3727 gated its retry on the
// plain-LOCAL target channel because that is the one whose verdict is a depth
// comparison. The `Dest::Field` / `Dest::OutParam` legs need no depth operand
// at all — a field's object and an out/ref parameter's caller-frame location
// outlive EVERY frame of this method unconditionally, which is exactly what
// §28.3 clause 1 states — so the retry is widened to them, carrying the
// source-kind argument of fix #3746 (`LocalOrOwnedParam`) with it, and the
// enclosing frame's `owned` PARAMETERS are installed in the swapped-in seed
// because `ParameterSymbol::decl` is null and no depth map can hold one.
//
// SOUNDNESS DISCIPLINE. Every positive is paired with a structurally similar
// but spec-LEGAL negative the fix must not reject, and each negative is run
// through the AOT oracle for its VALUE — an accepted program that stopped
// computing the right answer is not an acceptance (LANE_RULES §6c). The
// boundaries pinned here are (a) the frame comparison in BOTH directions,
// (b) the within-frame depth comparison, (c) the source KIND (a plain borrow
// parameter carries no frame-bounded obligation), and (d) the source
// PROVENANCE (a receiver-sourced borrow is admissible, §28.3 clause 2).

#include "support/aot_support.hpp"  // runAot — the strict AOT oracle
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// `R` is an ordinary class, so `using var r := new R();` is an owned
// scope-bound local (§28.3) whose capture is a §28.9 borrow.
constexpr const char* kPrelude =
    "module M; import System; "
    "class R "
    "  private v: int := 5; "
    "  public method V(): int return this.v; end V; "
    "end R; ";

std::string wrap(const std::string& body) {
    return std::string(kPrelude) + "class C " + body +
        "  public static method Main(): int "
        "    Console.WriteLine(Run().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

bool rejectsWith(const std::string& program, const char* needle) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

// Compiles, links, runs; returns captured stdout with trailing newlines
// trimmed. A program that did not run returns a sentinel, so a row that stops
// compiling fails legibly rather than matching an empty string.
std::string runOut(const std::string& program) {
    auto r = runAot(program);
    if (!r.ran) return std::string("<did-not-run>");
    std::string out = r.stdoutText;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
}

// The METHOD-level diagnostics these fixes reach through a new frame and two
// new channels. Asserted verbatim: if a second spelling of any of these rules
// ever appears beside the one in `checkBorrowStoreEscape`, these rows fail.
constexpr const char* kClosureMsg =
    "closure capturing `r` is assigned to `f`, which outlives it";
constexpr const char* kPlainMsg =
    "borrow held by `holder` would outlive its source `r`";
constexpr const char* kFieldMsg =
    "borrow of `r` stored into a field would outlive its source";
constexpr const char* kFieldParamMsg =
    "borrow of `p` stored into a field would outlive its source";
constexpr const char* kOutParamMsg =
    "borrow of `r` written through an out/ref parameter would outlive its "
    "source";

// ------------------------------------------- POSITIVES: #3743 source frame --

// POSITIVE (§28.3 / §28.9, #3743): the filed repro. `r` is an owned
// scope-bound local of the ENCLOSING LAMBDA `outer`'s own body; the closure
// capturing it is assigned to `f`, declared at METHOD scope, and `f` is called
// twice after `outer` has returned — and therefore after `r`'s disposal.
// Measured on the parent: rc=0, no diagnostic, rc=139 at run.
BEZEL_TEST(lambda_body_escape_owned_3743,
           closure_over_enclosing_lambda_local_to_method_local_rejected_28_9) {
    const std::string p = wrap(
        "  public static method Run(): int "
        "    var f := lambda(): int => 0; "
        "    var outer := lambda(): int => do "
        "      using var r := new R(); "
        "      var mk := lambda(): int => do "
        "        f := lambda(): int => r.V(); "
        "        return 0; "
        "      end; "
        "      return mk(); "
        "    end; "
        "    var t := outer(); "
        "    return t + f() + f(); "
        "  end Run; ");
    BEZEL_EXPECT_TRUE(rejectsWith(p, kClosureMsg));
    BEZEL_EXPECT_TRUE(rejectsWith(p, "(§28.3 / §28.9"));
}

// POSITIVE (§28.3, #3743): the PLAIN-STORE spelling of the same frame axis —
// no closure at all. `checkBorrowStoreEscape` selects a different diagnostic
// for it, so the two spellings regress independently.
BEZEL_TEST(lambda_body_escape_owned_3743,
           plain_store_from_enclosing_lambda_local_rejected_28_3) {
    const std::string p = wrap(
        "  public static method Run(): int "
        "    var holder: R? := null; "
        "    var outer := lambda(): int => do "
        "      using var r := new R(); "
        "      var mk := lambda(): int => do "
        "        holder := r; "
        "        return 0; "
        "      end; "
        "      return mk(); "
        "    end; "
        "    return outer(); "
        "  end Run; ");
    BEZEL_EXPECT_TRUE(rejectsWith(p, kPlainMsg));
    BEZEL_EXPECT_TRUE(rejectsWith(p, "(§28.3 borrow-lifetime rule"));
}

// POSITIVE (§28.3 / §28.9, #3743): THREE lambda frames deep — the source is in
// the OUTERMOST lambda's body and the store is made two frames in. Pins that
// the seal is a chain of arbitrary length, not "one more frame than #3727".
BEZEL_TEST(lambda_body_escape_owned_3743,
           closure_over_grandparent_lambda_local_rejected_28_9) {
    const std::string p = wrap(
        "  public static method Run(): int "
        "    var f := lambda(): int => 0; "
        "    var outer := lambda(): int => do "
        "      using var r := new R(); "
        "      var mid := lambda(): int => do "
        "        var inner := lambda(): int => do "
        "          f := lambda(): int => r.V(); "
        "          return 0; "
        "        end; "
        "        return inner(); "
        "      end; "
        "      return mid(); "
        "    end; "
        "    var t := outer(); "
        "    return t + f(); "
        "  end Run; ");
    BEZEL_EXPECT_TRUE(rejectsWith(p, kClosureMsg));
}

// -------------------------------------- POSITIVES: #3744 destination channel --

// POSITIVE (§28.3 clause 1, #3744): the filed repro. The source is a
// method-scope owned local and the destination is a FIELD, stored from inside
// a lambda body. Measured on the parent: rc=0, no diagnostic, rc=139 at run.
BEZEL_TEST(lambda_body_escape_owned_3744,
           closure_over_method_local_stored_into_field_rejected_28_3) {
    const std::string p = std::string(kPrelude) +
        "class C "
        "  private h: () -> int := lambda(): int => 0; "
        "  public method Run(): int "
        "    var total := 0; "
        "    do "
        "      using var r := new R(); "
        "      var mk := lambda(): int => do "
        "        this.h := lambda(): int => r.V(); "
        "        return 0; "
        "      end; "
        "      total := mk(); "
        "    end; "
        "    return total + this.h(); "
        "  end Run; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    Console.WriteLine(c.Run().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_TRUE(rejectsWith(p, kFieldMsg));
    BEZEL_EXPECT_TRUE(rejectsWith(p, "(§28.3's borrowed-field store rule"));
}

// POSITIVE (§28.3 *Auto-drop (affine)*, #3744 + its recorded parameter note):
// the same field channel with an `owned` PARAMETER as the source. This is the
// row that fails if the enclosing-frame seed stops installing the frame's
// owned parameters — `ParameterSymbol::decl` is null, so no depth map holds
// one and the widened channel would resolve nothing.
BEZEL_TEST(lambda_body_escape_owned_3744,
           closure_over_owned_param_stored_into_field_rejected_28_3) {
    const std::string p = std::string(kPrelude) +
        "class Box "
        "  public h: () -> int := lambda(): int => 0; "
        "  public method Take(p: owned R): int "
        "    var mk := lambda(): int => do "
        "      this.h := lambda(): int => p.V(); "
        "      return 0; "
        "    end; "
        "    var t := mk(); "
        "    p.Dispose(); "
        "    return t; "
        "  end Take; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    var r := new R(); "
        "    var t := b.Take(move r); "
        "    Console.WriteLine((t + b.h()).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_TRUE(rejectsWith(p, kFieldParamMsg));
    BEZEL_EXPECT_TRUE(rejectsWith(
        p, "is an `owned` parameter, whose value THIS method disposes at its "
           "end"));
}

// POSITIVE (§28.3, #3744): the OUT/REF-PARAMETER destination. The parameter's
// storage is the CALLER's frame, so the written closure dangles the moment the
// method returns. Measured on the parent: rc=0, no diagnostic, rc=139 at run.
BEZEL_TEST(lambda_body_escape_owned_3744,
           closure_over_method_local_written_through_ref_param_rejected_28_3) {
    const std::string p = std::string(kPrelude) +
        "class C "
        "  public static method Fill(ref g: () -> int): int "
        "    do "
        "      using var r := new R(); "
        "      var mk := lambda(): int => do "
        "        g := lambda(): int => r.V(); "
        "        return 0; "
        "      end; "
        "      var q := mk(); "
        "    end; "
        "    return 0; "
        "  end Fill; "
        "  public static method Main(): int "
        "    var g: () -> int := lambda(): int => 1; "
        "    var z := Fill(ref g); "
        "    Console.WriteLine((z + g()).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_TRUE(rejectsWith(p, kOutParamMsg));
    BEZEL_EXPECT_TRUE(
        rejectsWith(p, "the parameter's location lives in the caller's frame"));
}

// POSITIVE (#3743 + #3744 COMPOSED): the source is in an ENCLOSING LAMBDA's
// body and the destination is a FIELD. Neither half alone reaches it, so this
// row is the one that fails if either is reverted.
BEZEL_TEST(lambda_body_escape_owned_3744,
           closure_over_enclosing_lambda_local_stored_into_field_rejected) {
    const std::string p = std::string(kPrelude) +
        "class C "
        "  private h: () -> int := lambda(): int => 0; "
        "  public method Run(): int "
        "    var outer := lambda(): int => do "
        "      using var r := new R(); "
        "      var mk := lambda(): int => do "
        "        this.h := lambda(): int => r.V(); "
        "        return 0; "
        "      end; "
        "      return mk(); "
        "    end; "
        "    return outer() + this.h(); "
        "  end Run; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    Console.WriteLine(c.Run().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_TRUE(rejectsWith(p, kFieldMsg));
}

// ------------------------------------------------------------ NEGATIVES ----

// NEGATIVE (§28.3, #3743) — the target at the SAME depth as the source, both
// in the ENCLOSING LAMBDA's body. Byte-for-byte the first positive with `f`
// moved in beside `r`: they die at the same block end, so no call of `f` can
// be sequenced after `r`'s disposal. This pins the frame-chain comparison did
// NOT become "any store to a target outside the checked body is rejected" —
// the failure mode #3707's own first draft had.
BEZEL_TEST(lambda_body_escape_owned_3743,
           same_depth_target_in_enclosing_lambda_accepted_28_3) {
    BEZEL_EXPECT_EQ(runOut(wrap(
        "  public static method Run(): int "
        "    var outer := lambda(): int => do "
        "      using var r := new R(); "
        "      var f := lambda(): int => 0; "
        "      var mk := lambda(): int => do "
        "        f := lambda(): int => r.V(); "
        "        return 0; "
        "      end; "
        "      var t := mk(); "
        "      return t + f(); "
        "    end; "
        "    return outer(); "
        "  end Run; ")), "5");
}

// NEGATIVE (§28.3, #3743) — the frame comparison in the OTHER direction: the
// source is in the enclosing METHOD frame and the target is declared INSIDE
// the checked lambda body, so the target dies first. Accepted before this fix
// and deliberately still accepted (whether the lambda VALUE escapes is issue
// #3702's open half, recorded by fix #3564).
BEZEL_TEST(lambda_body_escape_owned_3743,
           method_local_source_with_inner_target_accepted_28_3) {
    BEZEL_EXPECT_EQ(runOut(wrap(
        "  public static method Run(): int "
        "    using var r := new R(); "
        "    var mk := lambda(): int => do "
        "      var f := lambda(): int => 0; "
        "      f := lambda(): int => r.V(); "
        "      return f(); "
        "    end; "
        "    return mk(); "
        "  end Run; ")), "5");
}

// NEGATIVE (§28.3, #3743) — a DEEPER target block inside the checked lambda
// body. The within-frame depth comparison must keep answering: a target at a
// strictly greater depth dies no later than the source.
BEZEL_TEST(lambda_body_escape_owned_3743,
           deeper_block_target_in_checked_body_accepted_28_3) {
    BEZEL_EXPECT_EQ(runOut(wrap(
        "  public static method Run(): int "
        "    var outer := lambda(): int => do "
        "      using var r := new R(); "
        "      var t := 0; "
        "      do "
        "        var f := lambda(): int => 0; "
        "        f := lambda(): int => r.V(); "
        "        t := f(); "
        "      end; "
        "      return t; "
        "    end; "
        "    return outer(); "
        "  end Run; ")), "5");
}

// NEGATIVE (§28.3 clause 1, #3744) — the SOURCE-KIND boundary of the widened
// field channel. `p` is a PLAIN (borrow) parameter: this method disposes
// nothing, so §28.3's admission of a "parameter-sourced" borrow holds and the
// store is the §28.5 back-reference idiom. The widened retry asks
// `symbolIsOwnedObligation`, so it must resolve no source here.
BEZEL_TEST(lambda_body_escape_owned_3744,
           plain_param_borrow_stored_into_field_accepted_28_3) {
    BEZEL_EXPECT_EQ(runOut(std::string(kPrelude) +
        "class Box "
        "  public back: R?; "
        "  public method Bind(p: R): int "
        "    var mk := lambda(): int => do "
        "      this.back := p; "
        "      return 0; "
        "    end; "
        "    return mk(); "
        "  end Bind; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var r := new R(); "
        "    using var b := new Box(); "
        "    Console.WriteLine((b.Bind(r) + b.back!.V()).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"), "5");
}

// NEGATIVE (§28.3 clause 2, #3744) — the SOURCE-PROVENANCE boundary. The
// stored borrow is reached through the RECEIVER's own owned field, whose
// lifetime is the receiver's, so no frame of this method bounds it.
BEZEL_TEST(lambda_body_escape_owned_3744,
           receiver_sourced_field_store_accepted_28_3) {
    BEZEL_EXPECT_EQ(runOut(std::string(kPrelude) +
        "class Box "
        "  public owned a: R?; "
        "  public back: R?; "
        "  public method Link(): int "
        "    var mk := lambda(): int => do "
        "      this.back := this.a; "
        "      return 0; "
        "    end; "
        "    return mk(); "
        "  end Link; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    b.a := new R(); "
        "    Console.WriteLine((b.Link() + b.back!.V()).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"), "5");
}

// NEGATIVE (§28.3, #3707's review lesson carried forward) — the target is a
// FOR-LOOP induction variable, a decl kind `collectLocalDepthsStmt` records in
// NO frame (`LocalSymbol::decl` is the `ForStmt` itself). The frame-chain
// comparison decides membership POSITIVELY, so it must decline here rather
// than read the miss as "declared outside the checked body".
BEZEL_TEST(lambda_body_escape_owned_3743,
           for_loop_variable_target_declines_28_3) {
    BEZEL_EXPECT_EQ(runOut(wrap(
        "  public static method Run(): int "
        "    var total := 0; "
        "    var outer := lambda(): int => do "
        "      using var r := new R(); "
        "      using var items := new Array<R>(); "
        "      items.Add(r); "
        "      for var cur in items do "
        "        var mk := lambda(): int => do "
        "          cur := r; "
        "          return 0; "
        "        end; "
        "        total := total + mk() + cur.V(); "
        "      end for; "
        "      return total; "
        "    end; "
        "    return outer(); "
        "  end Run; ")), "5");
}

// NEGATIVE (§28.3, #3744) — a lambda body that stores into a field a value
// carrying NO owned obligation at all (a fresh `new R()` handed to an `owned`
// field). Pins that the widened channel did not become "any field store made
// inside a lambda body is rejected".
BEZEL_TEST(lambda_body_escape_owned_3744,
           fresh_owned_value_into_owned_field_accepted_28_5) {
    BEZEL_EXPECT_EQ(runOut(std::string(kPrelude) +
        "class Box "
        "  public owned a: R?; "
        "  public method Fill(): int "
        "    var mk := lambda(): int => do "
        "      this.a := new R(); "
        "      return 0; "
        "    end; "
        "    return mk(); "
        "  end Fill; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    Console.WriteLine((b.Fill() + b.a!.V()).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"), "5");
}

}  // namespace
