// fix #6452 (codegen; language spec §28.3 *Returning a deferred-activation
// frame* / §29.2 *Desugaring of `for`* / §29.1 *`Current` semantics on
// borrowing and owning enumerators* / §28.6 *Leak prevention*; stdlib §4.5 /
// §4.13, v0.338): `return recv.Drain();` at a mint-announcing head produced a
// NULL return value behind a clean compile.
//
// THE BUG. `lowerCallExpr` has no `Drain` arm — only the for-in desugaring and
// the GetOwningEnumerator dispatch recognise the shape, and both do it by
// walking BACK to the initializer. At the RETURN site there is no
// initializer, so the call lowered to nothing, `v` stayed null, the §29.2
// return coercion was gated on `v && rs->value` and never ran, and
// `emitReturn`'s `v ? v : null` fallback handed the caller a null frame.
//
// MEASURED at b743b795, AOT + ASan, on the row (A) program below:
//
//   warning: internal: return-value expression failed to lower to a value
//   (degraded to a zero/null return value) — #2491
//   …
//   AddressSanitizer: SEGV on unknown address 0x000000000000
//   ==3694964==  in bz$M.P$Run
//
// The `#2491` note is a **Warning** by design — the §3 / #605 permissive
// posture keeps it one, because §3's `return o.Anything();` over an `Object`
// receiver reaches the same site legally. So the compile was CLEAN and the
// program crashed in user code, which is the worst failure mode a language
// whose selling point is reliable ownership can present.
//
// WHY A REAL VALUE AND NOT A DIAGNOSTIC. §28.3: "A non-iterator method may
// therefore compose and return such a frame ... provided every borrow the
// returned frame carries is sourced from the method's own parameters or
// receiver (directly or through the receiver's fields, the receiver-sourced
// notion of §28.6)". Row (A) is exactly the receiver-sourced case, and it is
// pinned as ADMITTED — `drain_view_return_site_2175.return_this_field_drain_
// admitted_control_spec_28_3` asserts `hasErrors() == false` over the same
// method. The scope-bound forms §28.3 REJECTS (`recv.Drain()` over a `using
// var`, a plain owned local, an `owned` parameter) are rejected by the
// ownership pass at the return site — rows
// `return_using_var_drain_rejected_spec_28_3`,
// `return_using_var_field_drain_chain_rejected_spec_28_3` and
// `return_plain_owned_local_drain_rejected_spec_28_3` — so they never reach
// codegen and nothing here re-opens them.
//
// ROWS.
//   (A) THE DEFECT — `public method View(): IOwningEnumerable<R> return
//       Items.Drain(); end View;` over an `owned OwningArray<R>` field, the
//       caller iterating the result. Pre-fix: exit 1, ASan SEGV on address 0.
//       Post-fix: exit 0, ASan/LSan clean, two elements observed.
//   (B) THE WORKING REPLACEMENT — the same program with `var v :=
//       Items.Drain(); return v;`. This is the shape #6452's issue records as
//       the workaround ("bind to a local first … that path lowers correctly"),
//       so it must keep working AND keep agreeing with (A). Pre- and
//       post-fix alike: exit 0, the same output.
//   (C) THE LIFO BACKING — the same return edge over an `OwningStack<R>`,
//       which takes `owningDrainWrapForHead`'s `ArrayLifo` leaf rather than
//       (A)'s `ArrayForward`. A fix that hard-coded the forward-array leaf
//       would pass (A) and (B) and iterate this one in the WRONG ORDER, so
//       the row pins the order, not just the count.
//   (D) THE NOT-YET-ADMITTED SIBLING — the BLOCK-bodied getter form. §28.6
//       rejects it ("a property or indexer getter may not return a
//       freshly-produced owned value — a getter is a borrow-producing
//       position"), the same rule the expression-form sibling is rejected by
//       in `drain_view_getter_2175.getter_drain_over_this_field_rejected_
//       spec_28_6`. #6452's issue records that widening the §28.6 carve to the
//       block form made this shape compile and then SEGV, which is what got
//       #6431's `6073bc0e` rejected. This row pins that the block form is
//       STILL rejected, so a future carve lands on a lowering that produces a
//       value rather than on the null this fix removed.
//
// NOT ESTABLISHED HERE, and reported with the fix: the fresh-producer
// receiver (`return MakePool().Drain();`) is left to `lowerExpr`, exactly as
// the local-decl face leaves it — re-lowering it would evaluate the producer
// twice. Its runtime behaviour is unchanged by this fix (still the #2491
// degrade), and the dictionary head (`OwningDictionary.Drain()`) is excluded
// by the same `isDictionaryName` test the local-decl face carries, because
// its drain yields `DrainedEntry<K, V>` through its own machinery.

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"          // runAotChecked — AOT + ASan/LSan
#include "support/ownership_support.hpp"    // parseAndCheckOwnership
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::checkedToolchainSkipped;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// A file-local substring test rather than a shared helper: #3892's note
// records that `has` has already been spelled per-file in this suite, and a
// new `bezel::test::has` would be the exact-match ambiguity that note warns
// about for the `hasErrorContaining` name.
bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// ctor +1 / dtor -1 net-zero dispose-once oracle. A double-free or a missed
// element disposal additionally fires ASan/LSan under `runAotChecked`
// (`r.ran == false`).
//
// The static is a `string`-typed class INSTANCE rather than an int: the §22.12
// rule-2 form-(iii) declaration-site warning on a mutable primitive static is
// noise this file has no reason to absorb, and `Console.WriteLine` on it keeps
// the element count observable through stdout.
const char* kR =
    "module M; "
    "class R "
    "  public static live: int := 0; "
    "  public static method Live(): int return R.live; end Live; "
    "  public constructor() R.live := R.live + 1; end constructor; "
    "  destructor R.live := R.live - 1; end destructor; "
    "  public method Tag(): string return \"r\"; end Tag; "
    "end R; ";

// Two seeded elements, so the count is 2 and NOT satisfiable by an empty or
// null frame returning zero — that is what keeps (A) from being vacuous.
const char* kSeed =
    "  public method Seed(): int "
    "    Items.Add(new R()); "
    "    Items.Add(new R()); "
    "    return 0; "
    "  end Seed; ";

}  // namespace

// ===========================================================================
// (A) THE DEFECT (§28.3 / §29.2 / stdlib §4.13) — a receiver-sourced drain
// frame returned from a plain method must reach the caller as a real frame.
// Pre-fix: the `#2491` internal warning and a null-page SEGV in the CALLER.
// ===========================================================================
BEZEL_TEST(drain_view_return_frame_lowered_6452,
           this_field_drain_returned_to_caller_iterates_spec_28_3) {
    std::string src = std::string(kR) +
        "class Owner "
        "  public owned Items: OwningArray<R>; "
        "  public constructor() Items := new OwningArray<R>(); end constructor; " +
        kSeed +
        "  public method View(): IOwningEnumerable<R> "
        "    return Items.Drain(); "
        "  end View; "
        "end Owner; "
        "class P "
        "  public static method Run(): int "
        "    using var h := new Owner(); "
        "    h.Seed(); "
        "    var n := 0; "
        "    using var view: IOwningEnumerable<R> := h.View(); "
        "    for var r in view do "
        "      n := n + 1; "
        "    end for; "
        "    Console.WriteLine($\"{n}\"); "
        "    return R.Live(); "
        "  end Run; "
        "  public static method Main(): int "
        "    return P.Run(); "
        "  end Main; "
        "end P;";
    JitResult r = runAotChecked(src);
    // `r.ran` false here is the null-page SEGV, reported by `runAot` as a
    // forced-ASan / signal diagnostic at SourceLocation{} — the reason this
    // file reads the verdict through `checkedToolchainSkipped` first rather
    // than accepting `ran` bare (#3892: `ran` is false for four outcomes).
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // TWO elements. A null or empty frame yields 0, so this line is the
    // non-vacuity witness for the whole row.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "2"));
    // The dispose balance: ctor +2, loop auto-drop -2, and the holder's own
    // `OwningArray` cascade finds an EMPTY backing. `R.Live()` is 0 exactly
    // when every element was discharged exactly once.
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (B) THE WORKING REPLACEMENT — the workaround #6452's issue names. It must
// keep working, and it must AGREE with (A): same elements, same count, same
// dispose balance. This is the pairing control for (A) — the two programs
// differ by the smallest legal edit, so a fix that only worked for the
// new shape could not pass both.
// ===========================================================================
BEZEL_TEST(drain_view_return_frame_lowered_6452,
           local_bound_then_returned_agrees_spec_28_3) {
    std::string src = std::string(kR) +
        "class Owner "
        "  public owned Items: OwningArray<R>; "
        "  public constructor() Items := new OwningArray<R>(); end constructor; " +
        kSeed +
        "  public method View(): IOwningEnumerable<R> "
        "    var v := Items.Drain(); "
        "    return v; "
        "  end View; "
        "end Owner; "
        "class P "
        "  public static method Run(): int "
        "    using var h := new Owner(); "
        "    h.Seed(); "
        "    var n := 0; "
        "    using var view: IOwningEnumerable<R> := h.View(); "
        "    for var r in view do "
        "      n := n + 1; "
        "    end for; "
        "    Console.WriteLine($\"{n}\"); "
        "    return R.Live(); "
        "  end Run; "
        "  public static method Main(): int "
        "    return P.Run(); "
        "  end Main; "
        "end P;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "2"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (C) THE LIFO BACKING (§29.1 / stdlib §4.11) — the same return edge over an
// `OwningStack<R>`, which takes `owningDrainWrapForHead`'s `ArrayLifo` leaf
// instead of (A)'s `ArrayForward`. The row pins the ORDER, so a fix that
// hard-coded the forward leaf fails it. Stack §4.11 iteration order is
// top-to-bottom, and the two pushes put `r0` on top.
// ===========================================================================
BEZEL_TEST(drain_view_return_frame_lowered_6452,
           owning_stack_drain_return_keeps_lifo_order_spec_29_1) {
    std::string src =
        "module M; "
        "class R "
        "  public static live: int := 0; "
        "  public Tag: string; "
        "  public constructor(tag: string) Tag := tag; R.live := R.live + 1; end constructor; "
        "  destructor R.live := R.live - 1; end destructor; "
        "end R; "
        "class Owner "
        "  public owned Items: OwningStack<R>; "
        "  public constructor() Items := new OwningStack<R>(); end constructor; "
        "  public method Seed(): int "
        "    Items.Push(new R(\"zero\")); "
        "    Items.Push(new R(\"one\")); "
        "    return 0; "
        "  end Seed; "
        "  public method View(): IOwningEnumerable<R> "
        "    return Items.Drain(); "
        "  end View; "
        "end Owner; "
        "class P "
        "  public static method Run(): int "
        "    using var h := new Owner(); "
        "    h.Seed(); "
        "    var line := \"\"; "
        "    using var view: IOwningEnumerable<R> := h.View(); "
        "    for var r in view do "
        "      line := line + r.Tag; "
        "    end for; "
        "    Console.WriteLine(line); "
        "    return R.live; "
        "  end Run; "
        "  public static method Main(): int "
        "    return P.Run(); "
        "  end Main; "
        "end P;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // Top-to-bottom: the LAST push drains FIRST. Forward order would print
    // "zeroone" and this line would go red.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "onezero"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (D) THE NOT-YET-ADMITTED SIBLING (§28.6) — the BLOCK-bodied getter form of
// the same shape stays REJECTED at compile time, by the §28.6 accessor
// borrow-producing rule its expression-form sibling is rejected by. #6452's
// issue records that admitting it made the program compile and then SEGV,
// which is what got #6431's `6073bc0e` rejected; this row is the standing
// proof that the rejection is still there after the lowering fix, so a future
// carve meets a real frame rather than the null this fix removed.
// ===========================================================================
BEZEL_TEST(drain_view_return_frame_lowered_6452,
           block_getter_drain_return_still_rejected_spec_28_6) {
    auto r = parseAndCheckOwnership(
        std::string(kR) +
        "class Holder "
        "  private owned _items: OwningArray<R>; "
        "  public constructor() _items := new OwningArray<R>(); end constructor; "
        "  public property Q: IOwningEnumerable<R> "
        "    get => do "
        "      return _items.Drain(); "
        "    end; "
        "  end Q; "
        "end Holder; "
        "class P "
        "  public static method Run(): int "
        "    using var h := new Holder(); "
        "    using var view: IOwningEnumerable<R> := h.Q; "
        "    return R.Live(); "
        "  end Run; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("freshly-produced owned value") != std::string::npos) {
            found = true;
            break;
        }
    }
    BEZEL_EXPECT_TRUE(found);
}

// ===========================================================================
// (E) THE ADMITTED CONTROL STILL COMPILES (§28.3) — the exact program
// `drain_view_return_site_2175.return_this_field_drain_admitted_control_
// spec_28_3` pins as admitted is also the one whose lowering this fix
// changed. This row asserts the compile stays clean at the ownership layer:
// if #6452's arm had reached one of the scope-bound forms, this would go red.
// (It is the semantic half of (A); (A) is the runtime half.)
// ===========================================================================
BEZEL_TEST(drain_view_return_frame_lowered_6452,
           this_field_drain_return_still_no_errors_spec_28_3) {
    auto r = parseAndCheckOwnership(
        std::string(kR) +
        "class Owner "
        "  public owned Items: OwningArray<R>; "
        "  public constructor() Items := new OwningArray<R>(); end constructor; "
        "  public method View(): IOwningEnumerable<R> "
        "    return Items.Drain(); "
        "  end View; "
        "end Owner;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}