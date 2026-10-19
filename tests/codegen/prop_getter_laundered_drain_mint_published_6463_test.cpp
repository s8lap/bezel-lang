// fix #6463 (codegen / soundness; language spec §29.2 *Desugaring of `for`* /
// §28.3 *Borrow lifetime rules* / §28.9 *Runtime-managed types* / §4.5
// (stdlib) — the `kPropGetterReturnMint` verdict was INVERTED on the
// LAUNDERED block-getter spelling, and the inversion cost a live leak.
//
// THE SHAPE. A block-bodied getter that binds the drain frame to a local and
// returns the local:
//
//     public property Q: IOwningEnumerable<R>
//       get => do
//         var v := _items.Drain();    // <- local-decl face MINTS a real frame
//         return v;                   // <- §29.2 coercion DECLINES (v has the head)
//       end;
//     end Q;
//
// read at a local-decl bind site:
//
//     var e: IOwningEnumerable<R> := h.Q;
//     for var r in e do ... end for;
//
// WHY THE VERDICT WAS INVERTED. The frame is minted by the LOCAL-DECL face
// (`lowerLocalDecl`'s #4750 / #4772 gate calling `wrapAsIEnumerableForTarget`),
// one statement EARLIER — so by the time `lowerReturn`'s own §29.2 return-edge
// coercion runs, the value already carries the declared `IOwningEnumerable<T>`
// head and the call DECLINES. #6397's publication reads only that call's
// `outWrapMade`, so it published "no frame was minted" over a frame that
// really was built.
//
// THE CONSEQUENCE — ESTABLISHED, NOT ASSUMED, AND IT IS (ii), NOT (i). The
// reader `Setup.cpp`'s #6405 `ownsSource` arm asks that verdict to decide
// whether the bound local claims the frame. Told "no mint", it took the LEND
// branch and claimed nothing; the for-in over a LocalSymbol registers no
// loop-end dispose; and nothing else frees the frame. MEASURED pre-fix, AOT +
// ASan/LSan, this file's rows (A)/(B)/(C)/(D):
//
//   var e := h.Q; for r in e do ... end;   -> Direct leak of 32 byte(s) in
//                                             1 object(s) allocated from
//                                             bz$prop$Q$get
//   200 reads of the same shape            -> Direct leak of 6400 byte(s) in
//                                             200 object(s) — the 32-byte
//                                             `{vtable, arr, pending}` frame
//
// The frame is the #6385 / #3385 `wrapArrayAsIOwningEnumerator` draining frame
// (32 bytes, three pointer slots) and it is BORROWING (stdlib §4.5: "disposing
// the drain view does not dispose the array"), so the leak is the frame alone —
// the holder's `OwningArray` cascade still finds and frees its own backing.
//
// WHY THE DIRECT SPELLING IS NOT "ALSO FIXED" HERE, AND WHY THAT IS NOT A
// GAP IN THIS FIX. The unlaundered `return _items.Drain();` is REJECTED at
// compile time by §28.6's accessor borrow-producing rule (measured at #6404;
// pinned by `drain_view_return_frame_lowered_6452.
// block_getter_drain_return_still_rejected_spec_28_6` and
// `drain_return_block_getter_rejected_28_6_6452.bzl`), while the laundered
// form is admitted. The asymmetry is DELIBERATE and recorded in the compiler's
// own source: the comment above `getterEscapeIsMintAnnouncingFrame`'s carve
// reads "⚠ THE DIRECT SPELLING OF THIS SHAPE IS STILL REJECTED TODAY … the
// compiler is INTERNALLY INCONSISTENT — direct reject, laundered accept."
// Whether §28.6's carve should reach the laundered form is a SPEC question,
// not a codegen fix, and this change does not touch which spellings are
// admitted — row (G) below pins the rejection so a future carve lands on this
// lowering rather than on the null #6452 removed.
//
// ROWS.
//
//   (A) THE DEFECT, amplified. 200 reads through a local-decl bind. Pre-fix:
//       `Direct leak of 6400 byte(s) in 200 object(s)`; post-fix: clean. The
//       amplification is what makes a per-read leak unmissable and satisfies
//       the 50-500x discipline for a one-shot leak oracle.
//   (B) THE SAME SHAPE, ONCE — the minimal repro, one read through a local-decl
//       bind, with the element count on stdout so an empty or null frame cannot
//       satisfy it.
//   (C) THE `const` BIND — §21.1's runtime-`const` spelling of the same reader.
//       It reaches `registerIterableSourceDisposal` through the same
//       `localDeclInitView` authority, and it leaked identically pre-fix, so it
//       is a separate reader of the same verdict rather than a restatement of
//       (B).
//   (D) THE REVERSE DIRECTION — the SAME amplified program read through a
//       DIRECT `for var r in h.Q`, and again through the already-minting
//       non-laundered getter `return _items;`. Both must stay CLEAN. This is
//       the half that catches a fix which buys the leak with a double-dispose:
//       the fix can only ever ADD a mint verdict, and these two rows are where
//       an over-broad verdict would show up. Both are amplified 200x, so a
//       double-dispose would be a heap-use-after-free under ASan on the first
//       iteration, not a marginal one.
//   (E) THE FIELD-LEND CONTROL — a block getter returning an `owned
//       IOwningEnumerable<R>` FIELD, the #6405 shape where the wrap DECLINES
//       and the holder keeps the obligation (§28.2). It must stay a lend: a
//       claim over this unminted frame frees the holder's field out from under
//       it, which is the measured #6405 heap-use-after-free in
//       `Holder$DisposeCore`. A fix that published a mint for every
//       identifier-returning block getter would go RED here.
//   (F) THE DIRECT-READ CONTROL — the same laundered getter read by a DIRECT
//       `for var r in h.Q`. Already clean pre-fix (the for-in's own
//       `!sourceIsNamedLocal` classification covers it whatever the map says),
//       and it is the pairing control for (A): the two programs differ by the
//       smallest legal edit, so a fix that only moved the reader's verdict
//       rather than the writer's could not tell them apart.
//   (G) THE DIRECT SPELLING IS STILL REJECTED (§28.6) — the standing proof
//       that this change did not widen what the ownership pass admits.

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

// File-local substring test: #3892's note records that `has` has already been
// spelled per-file in this suite, and a shared `bezel::test::has` would be the
// exact-match ambiguity that note warns about.
bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// ctor +1 / dtor -1 net-zero dispose-once oracle. A double-free or a missed
// element disposal additionally fires ASan/LSan under `runAotChecked`
// (`r.ran == false`).
//
// The static is a `string`-typed class INSTANCE rather than an int: the §22.12
// rule-2 form-(iii) declaration-site warning on a mutable primitive static is
// noise this file has no reason to absorb.
const char* kR =
    "module M; "
    "class R "
    "  public static live: int := 0; "
    "  public static method Live(): int return R.live; end Live; "
    "  public constructor() R.live := R.live + 1; end constructor; "
    "  destructor R.live := R.live - 1; end destructor; "
    "  public method Tag(): string return \"r\"; end Tag; "
    "end R; ";

// The Holder, the two-element seed, and the block-bodied getter whose body is
// SUPPLIED by the caller — so every row below is the same program with the
// smallest legal edit between them, which is what makes the pairing controls
// (D) and (F) mean anything.
std::string holderWithGetter(const std::string& getterBody) {
    return std::string(kR) +
        "class Holder "
        "  private owned _items: OwningArray<R>; "
        "  public constructor() _items := new OwningArray<R>(); end constructor; "
        "  public method Seed(): int "
        "    _items.Add(new R()); "
        "    _items.Add(new R()); "
        "    return 0; "
        "  end Seed; "
        "  public property Q: IOwningEnumerable<R> "
        "    get => do\n" +
        getterBody +
        "    end; "
        "  end Q; "
        "end Holder; ";
}

// The LAUNDERED getter body — `var v := _items.Drain(); return v;`. The local
// is declared WITHOUT a type annotation, so the drain face's own §29.2 gate
// (`v && declType && init`) is skipped and the frame's ownership rests on the
// #4750 / #4772 mint plus the reader's claim, exactly as in the measured
// repro. Annotating it would mint through a different arm.
const char* kLaunderedBody =
    "      var v := _items.Drain();\n"
    "      return v;\n";

// The NON-laundered minting body, `return _items;`, whose return-edge coercion
// DOES fire and so already published `kPropGetterReturnMint = true` pre-fix.
// It is the control that says the fix did not merely turn the verdict on.
const char* kMintBody = "      return _items;\n";

// The amplification. 200 reads through the reader under test.
//
// Each iteration RE-SEEDS before it reads, and that is load-bearing rather than
// decorative: `Drain()` EMPTIES its source (stdlib §4.5), so a single seed
// would leave the 199 later iterations draining an empty collection. Without
// the re-seed every iteration after the first would yield zero elements, the
// per-read LEAK would still occur, but the row's stdout witness could not tell
// a working loop from a broken one. Re-seeding makes each read yield its two
// elements, so `n=400` is a real witness that 200 reads each observed data.
std::string amplifiedReader(const std::string& readerStmt) {
    return "class P "
           "  public static method Run(): int "
           "    using var h := new Holder(); "
           "    var n := 0; "
           "    var i := 0; "
           "    while i < 200 do "
           "      h.Seed(); "
           "      " + readerStmt + " "
           "      for var r in SRC do "
           "        n := n + 1; "
           "      end for; "
           "      i := i + 1; "
           "    end; "
           "    Console.WriteLine($\"n={n}\"); "
           "    return R.Live(); "
           "  end Run; "
           "  public static method Main(): int "
           "    return P.Run(); "
           "  end Main; "
           "end P;";
}

// `SRC` is substituted by the caller: `e` when a local-decl bind is under test,
// `h.Q` when the direct read is.
std::string amplifiedProgram(const std::string& getterBody,
                             const std::string& readerStmt, const char* src) {
    std::string p = amplifiedReader(readerStmt);
    const std::string needle = "SRC";
    for (std::size_t at = p.find(needle); at != std::string::npos;
         at = p.find(needle, at))
        p.replace(at, needle.size(), src);
    return holderWithGetter(getterBody) + p;
}

}  // namespace

// ===========================================================================
// (A) THE DEFECT, AMPLIFIED (§29.2 / §28.3 / stdlib §4.5) — 200 reads of a
// laundered block getter through a local-decl bind must not leak the frame.
// Pre-fix: `Direct leak of 6400 byte(s) in 200 object(s)` from
// `bz$prop$Q$get`.
// ===========================================================================
BEZEL_TEST(prop_getter_laundered_drain_mint_published_6463,
           laundered_getter_localdecl_bind_no_leak_spec_29_2) {
    std::string src = amplifiedProgram(
        kLaunderedBody, "var e: IOwningEnumerable<R> := h.Q;", "e");
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 400 elements: two seeded, drained by each of the 200 reads. The count is
    // the non-vacuity witness — a null or empty frame would print 0 — and it
    // also pins that the loop really ran 200 times, which is what makes the
    // leak figure above reachable at all.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "n=400"));
    // The dispose balance. A double-dispose would have made `r.ran` false
    // already; this catches the opposite, a claim that fired without the frame
    // existing, which is the #6405 `Holder$DisposeCore` failure.
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (B) THE SAME SHAPE ONCE — the minimal repro, one read through the same
// reader. It leaked 32 bytes pre-fix. Kept as its own row because (A)'s
// amplification could in principle be satisfied by a fix that only handles the
// SECOND read (a memoised getter's verdict is published once per accessor
// emission), and this row pins the first.
// ===========================================================================
BEZEL_TEST(prop_getter_laundered_drain_mint_published_6463,
           laundered_getter_single_bind_no_leak_spec_29_2) {
    std::string src = holderWithGetter(kLaunderedBody) +
        "class P "
        "  public static method Run(): int "
        "    using var h := new Holder(); "
        "    h.Seed(); "
        "    var n := 0; "
        "    var e: IOwningEnumerable<R> := h.Q; "
        "    for var r in e do "
        "      n := n + 1; "
        "    end for; "
        "    Console.WriteLine($\"n={n}\"); "
        "    return R.Live(); "
        "  end Run; "
        "  public static method Main(): int "
        "    return P.Run(); "
        "  end Main; "
        "end P;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // TWO elements, from stdout. A null or empty frame yields 0.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "n=2"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (C) THE `const` BIND (§21.1) — the same verdict read through
// `registerIterableSourceDisposal`'s other decl kind. It reaches that function
// through the shared `localDeclInitView` authority (#3402), and it leaked
// identically pre-fix (measured `Direct leak of 32 byte(s)`), so it is a
// DISTINCT reader of the same published verdict rather than a restatement of
// (B): a fix that reached the plain `var` spelling but not the `const` one
// would pass (B) and go red here.
// ===========================================================================
BEZEL_TEST(prop_getter_laundered_drain_mint_published_6463,
           laundered_getter_const_bind_no_leak_spec_21_1) {
    std::string src = holderWithGetter(kLaunderedBody) +
        "class P "
        "  public static method Run(): int "
        "    using var h := new Holder(); "
        "    h.Seed(); "
        "    var n := 0; "
        "    const e: IOwningEnumerable<R> := h.Q; "
        "    for var r in e do "
        "      n := n + 1; "
        "    end for; "
        "    Console.WriteLine($\"n={n}\"); "
        "    return R.Live(); "
        "  end Run; "
        "  public static method Main(): int "
        "    return P.Run(); "
        "  end Main; "
        "end P;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "n=2"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (D) THE REVERSE DIRECTION (§28.2 / §28.3) — the SAME programs on the reader
// that was already correct must stay correct. Never trade a leak for a crash:
// the fix can only ADD a mint verdict, so these are the rows where an
// over-broad verdict would turn the leak into a heap-use-after-free, and they
// are amplified 200x so a double-dispose would fire on the FIRST iteration
// rather than hiding under the sanitizer's noise.
//
// (D1) the laundered getter read by a DIRECT `for`;
// (D2) the already-minting non-laundered getter read by a local-decl bind;
// (D3) the already-minting non-laundered getter read by a DIRECT `for`.
// ===========================================================================
BEZEL_TEST(prop_getter_laundered_drain_mint_published_6463,
           laundered_getter_direct_read_still_no_double_dispose_spec_29_2) {
    std::string src = amplifiedProgram(
        kLaunderedBody, "var ignored := 0;", "h.Q");
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "n=400"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(prop_getter_laundered_drain_mint_published_6463,
           already_minting_getter_bind_still_no_double_dispose_spec_29_2) {
    std::string src = amplifiedProgram(
        kMintBody, "var e: IOwningEnumerable<R> := h.Q;", "e");
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "n=400"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(prop_getter_laundered_drain_mint_published_6463,
           already_minting_getter_direct_still_no_double_dispose_spec_29_2) {
    std::string src = amplifiedProgram(kMintBody, "var ignored := 0;", "h.Q");
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "n=400"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (E) THE FIELD-LEND CONTROL (§28.2 / §29.2 / §28.9) — a block getter that
// hands back an `owned IEnumerable<int>` FIELD. Its return-edge coercion
// DECLINES (the value already HAS the declared head) and no frame is built, so
// the holder keeps the obligation and the reader must take the LEND branch.
//
// This is the row that fails in the OTHER direction. #6405's own measured
// consequence of claiming over this unminted frame is a heap-use-after-free —
// `Holder$DisposeCore` reading its own freed `_seq` field — so a fix that
// published a mint for EVERY identifier-returning block getter, rather than
// only for one whose local the wrap creator demonstrably built, goes red here.
// The authority the fix consults (`iterFormatWrappedDecls_`, the local-decl
// face's own `v != preIterWrap` self-report) is a member of NO decl in this
// shape — the getter body has no local at all — so the verdict stays "no mint".
//
// The backing is an `Array<int>`, which is #6405's own decline shape and is
// byte-compatible with that file's rows so a failure here is comparable. The
// `IOwningEnumerable<R>`-field variant of the same idea is NOT usable here: it
// SEGVs in `bz$M.P$Run` on address 0x0 IDENTICALLY on this branch's parent and
// on this fix, which is a separate pre-existing defect this row neither pins
// nor depends on.
// ===========================================================================
BEZEL_TEST(prop_getter_laundered_drain_mint_published_6463,
           field_lend_block_getter_still_a_lend_spec_28_2) {
    // 16 iterations × (3+4) = 112. The exit code is the oracle rather than a
    // stdout count because the elements are `int`s with no disposal obligation
    // of their own; the frame they sit in is what ASan would catch.
    std::string src =
        "module M; "
        "class HolderI "
        "  private owned _seq: IEnumerable<int>; "
        "  public constructor() "
        "    var a := new Array<int>(); "
        "    a.Add(3); "
        "    a.Add(4); "
        "    _seq := a; "
        "  end constructor; "
        "  public property Q: IEnumerable<int> "
        "    get => do "
        "      return _seq; "
        "    end; "
        "  end Q; "
        "end HolderI; "
        "class P "
        "  public static method Run(): int "
        "    var total := 0; "
        "    for var i in 1..16 do "
        "      using var h := new HolderI(); "
        "      var e: IEnumerable<int> := h.Q; "
        "      for var x in e do total := total + x; end; "
        "    end; "
        "    Console.WriteLine($\"total={total}\"); "
        "    if total = 112 then return 0; end; "
        "    return 1; "
        "  end Run; "
        "  public static method Main(): int "
        "    return P.Run(); "
        "  end Main; "
        "end P;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // 112 = 16 × 7, so the holder's field really was readable through all 16
    // iterations — a claim that freed it early would have taken `r.ran` false
    // with a `Holder$DisposeCore` read of freed memory, and a claim that never
    // happened at all would still print 112 but leak, which LSan would catch.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=112"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (G) THE DIRECT SPELLING IS STILL REJECTED (§28.6) — the standing proof that
// this change did not widen what the ownership pass admits. The unlaundered
// `return _items.Drain();` is §28.6's borrow-producing-accessor rejection
// (measured at #6404), and #6452's issue records that admitting it made the
// program compile and then SEGV, which is what got #6431's `6073bc0e`
// rejected. Row (D)'s fix lands on a lowering that produces a real frame, so a
// future carve has something sound to admit.
//
// This is a SCOPE-CLAIM PIN, not a behavioural one: `parseAndCheckOwnership`
// runs Lexer → Parser → … → checkOwnership and returns, never reaching codegen.
// It is green at base, pre-fix and post-fix alike, and it is kept because what
// it protects is the ASYMMETRY, which is a deliberate property of the compiler
// rather than an accident of this change.
// ===========================================================================
BEZEL_TEST(prop_getter_laundered_drain_mint_published_6463,
           direct_getter_drain_still_rejected_spec_28_6) {
    auto r = parseAndCheckOwnership(holderWithGetter(
        "      return _items.Drain();\n") +
        "class P "
        "  public static method Run(): int "
        "    using var h := new Holder(); "
        "    h.Seed(); "
        "    var e: IOwningEnumerable<R> := h.Q; "
        "    return R.Live(); "
        "  end Run; "
        "  public static method Main(): int "
        "    return P.Run(); "
        "  end Main; "
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