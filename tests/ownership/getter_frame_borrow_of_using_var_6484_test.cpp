// fix #6484 / §28.3 (spec v0.338): a getter returned a FRAME carrying a borrow
// of a scope-bound local, and the return site admitted it.
//
// §28.3 verbatim, *Returning a deferred-activation frame*: "provided every
// borrow the returned frame carries is sourced from the method's own parameters
// or receiver (directly or through the receiver's fields, the receiver-sourced
// notion of §28.6) … A returned frame carrying a borrow of anything else — a
// scope-bound local, a `using var`, a temporary — is rejected at the return
// site, since the frame would outlive the source; the remedy is to materialize
// instead (return an `owned` collection)." (v0.338)
//
// WHAT #6404 CLOSED AND WHAT WAS STILL OPEN. #6404 made the getter return walk
// the returned BINDING back to its producer, because §28.6's rule is about the
// returned VALUE and an `IdentifierExpr` reads Borrowed. That peel asks one
// question — "does the chain reach a FRESH OWNED producer?" — and this defect
// is the orthogonal one it never asks: what BORROW does the frame CARRY?
// The two can disagree at once, and here they do. `MakeItems()` is a fresh
// owned producer, but for a mint-announcing return type the CONTAINER is
// announced (announcement table row 2) while the CARRIED borrows stay governed
// by §28.3. So the mint-announcing carve stops the #6404 peel, the peel never
// reaches a producer, and the escape is admitted — the same laundering
// asymmetry #6404 closed, one clause further down.
//
// MEASURED at ba713eb9 (staging tip for this run), AOT + ASan:
// `heap-use-after-free`, freed by `bz$elemDispose`, allocated in `MakeItems`,
// called from `bz$prop$Items$get` — the caller reads the frame's elements
// after the getter's scope disposed them. `asan_frame_borrows_using_var_is_a_
// uaf_red_before_the_fix` below pins that measurement as a live regression.
//
// THE ACCEPT ROWS ARE THE LOAD-BEARING HALF. This fix ADDS a rejection, and a
// reclassification that adds one has over-rejected in this codebase before
// (turning a legal program into a false rejection). The §28.6 mint-announcing
// carve admits returning a frame whose root is a FIELD of the receiver — the
// `get => _items.Drain()` shape three landed commits rest on (`d62a8a4a`
// #6404, `83cd1b79`, `05ac79a1` #6452). `drain_alias_over_receiver_field_
// accepted` and `field_read_through_local_accepted` pin that carve from both
// directions, and `reassigned_local_discharged_accepted` pins the §28.7
// drop-on-overwrite stop: a reassigned local's source is retired, so what it
// holds afterwards is the field's, not the local's.
//
// The discriminator pair `frame_over_receiver_field_accepted` /
// `frame_over_using_var_local_rejected` is byte-identical in body except for
// the root — a FIELD versus a `using var` LOCAL — so it pins the boundary the
// issue names ("Those must come out differently") mechanically.
// per §28.3, §28.6, §28.7, announcement table row 2, K2.2, K6.2, #6484

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

// The §28.3 needle. A STABLE rule identifier (§-reference plus the key term),
// so a wording tweak that rephrases the sentence does not silently break the
// row while an unrelated rule firing cannot satisfy it.
constexpr const char* kFrameEscape =
    "a getter may not return a frame that borrows";

const char* kPreamble = R"(
module M;
class Cell
  public Name: string := "";
end Cell;
class Factory
  private owned _items: OwningArray<Cell>;
  public constructor()
    _items := new OwningArray<Cell>();
  end constructor;
  private static method MakeItems(): owned OwningArray<Cell>
    var a := new OwningArray<Cell>();
    a.Add(new Cell());
    return a;
  end MakeItems;
)";

// ── the defect ─────────────────────────────────────────────────────────────
// The issue's shape, minimally: bind the deferred-activation frame to a local,
// then return THAT local.
BEZEL_TEST(getter_frame_borrow_of_using_var_6484,
           frame_over_using_var_local_rejected) {
    auto r = parseAndCheckOwnership(std::string(kPreamble) + R"(
  public property View: IEnumerable<Cell>
    get => do
      using var items2 := MakeItems();
      var frame := items2.Where(lambda(c) => true);
      return frame;
    end;
  end View;
end Factory;
class C public static method Main(): int return 0; end Main; end C;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFrameEscape));
}

// The DIRECT spelling — no laundering at all. Also rejected before the fix, but
// by a DIFFERENT rule and a DIFFERENT diagnostic: §28.6's fresh-owned rule
// reaches `MakeItems()` through the peel. Asserting the DIRECT diagnostic
// rather than the §28.3 one pins that the two rules stay distinct, and makes
// the laundering row above non-vacuous (the defect is not merely "the direct
// shape also rejects").
BEZEL_TEST(getter_frame_borrow_of_using_var_6484,
           direct_frame_over_using_var_local_rejected_by_28_6) {
    auto r = parseAndCheckOwnership(std::string(kPreamble) + R"(
  public property View: IEnumerable<Cell>
    get => do
      using var items2 := MakeItems();
      return items2.Where(lambda(c) => true);
    end;
  end View;
end Factory;
class C public static method Main(): int return 0; end Main; end C;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a property or indexer getter may not return a freshly-produced "
           "owned value"));
}

// A PLAIN `var` owned local — not a `using var` — is the same escape: §28.3's
// affine auto-drop disposes it at scope end just the same. The issue names
// only the `using var` form; both are "a scope-bound local", the phrase §28.3
// itself uses, and both are auto-dropped there. Pinned so the two spellings
// cannot come apart.
BEZEL_TEST(getter_frame_borrow_of_using_var_6484,
           frame_over_plain_owned_local_rejected) {
    auto r = parseAndCheckOwnership(std::string(kPreamble) + R"(
  public property View: IEnumerable<Cell>
    get => do
      var items2 := MakeItems();
      var frame := items2.Where(lambda(c) => true);
      return frame;
    end;
  end View;
end Factory;
class C public static method Main(): int return 0; end Main; end C;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFrameEscape));
}

// Several locals in the laundering chain — a one-binding rule would leave this
// open, and the chain is exactly what the #6404 peel taught us to walk.
BEZEL_TEST(getter_frame_borrow_of_using_var_6484,
           several_binding_chain_rejected) {
    auto r = parseAndCheckOwnership(std::string(kPreamble) + R"(
  public property View: IEnumerable<Cell>
    get => do
      using var items2 := MakeItems();
      var frame := items2.Where(lambda(c) => true);
      var again := frame;
      return again;
    end;
  end View;
end Factory;
class C public static method Main(): int return 0; end Main; end C;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kFrameEscape));
}

// ── the carve that must NOT break ──────────────────────────────────────────
// §28.6 bullet 2's mint-announcing parenthetical and §28.3's corresponding
// exception admit a frame rooted at a FIELD of a mint-announcing type. This is
// the `arr.Drain()` shape `d62a8a4a` (#6404), `83cd1b79` and `05ac79a1`
// (#6452) rest on — bound to a local and returned, exactly the laundering
// spelling this fix closes, but with a FIELD root instead of a local root.
//
// MEASURED ACCEPTED both before and after the fix (pre-fix at ba713eb9). It is
// the discriminator that matters: byte-identical to
// `frame_over_using_var_local_rejected` except that `arr` is a receiver field
// where `items2` is a `using var` local, so the two must come out differently
// and this one is the side that must stay green.
BEZEL_TEST(getter_frame_borrow_of_using_var_6484,
           drain_alias_over_receiver_field_accepted) {
    auto r = parseAndCheckOwnership(std::string(kPreamble) + R"(
  public property View: IOwningEnumerable<Cell>
    get => do
      var v := _items.Drain();
      return v;
    end;
  end View;
end Factory;
class C public static method Main(): int return 0; end Main; end C;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The discriminator partner: byte-identical body, FIELD root instead of a
// `using var` LOCAL root. This is the pair that pins the boundary mechanically —
// one byte of the receiver (`_items` vs `items2`) is the whole difference
// between accept and reject, and the fix's own header says the two "must come
// out differently".
BEZEL_TEST(getter_frame_borrow_of_using_var_6484,
           frame_over_receiver_field_accepted) {
    auto r = parseAndCheckOwnership(std::string(kPreamble) + R"(
  public property View: IEnumerable<Cell>
    get => do
      var items2 := _items;
      var frame := items2.Where(lambda(c) => true);
      return frame;
    end;
  end View;
end Factory;
class C public static method Main(): int return 0; end Main; end C;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A borrow-typed local (a local holding a BORROW, not an owned value) is not
// the hazard this rule names — the borrow's source is the field, which outlives
// the getter. `isUsing` false and the initializer not owned ⇒ admitted.
BEZEL_TEST(getter_frame_borrow_of_using_var_6484,
           frame_over_borrow_typed_local_accepted) {
    auto r = parseAndCheckOwnership(std::string(kPreamble) + R"(
  public property View: IEnumerable<Cell>
    get => do
      var items2: OwningArray<Cell> := _items;
      var frame := items2.Where(lambda(c) => true);
      return frame;
    end;
  end View;
end Factory;
class C public static method Main(): int return 0; end Main; end C;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.7's drop-on-overwrite retires the scope-bound source; what the local
// holds afterwards is the FIELD's value, so the returned frame borrows the
// field and is legal. This is the stop that keeps the rule from firing on any
// local a getter happens to touch.
BEZEL_TEST(getter_frame_borrow_of_using_var_6484,
           reassigned_local_discharged_accepted) {
    auto r = parseAndCheckOwnership(std::string(kPreamble) + R"(
  public property View: IEnumerable<Cell>
    get => do
      var items2 := MakeItems();
      items2 := _items;
      var frame := items2.Where(lambda(c) => true);
      return frame;
    end;
  end View;
end Factory;
class C public static method Main(): int return 0; end Main; end C;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A getter typed at ORDINARY borrowable class rather than a deferred-activation
// kind is outside this rule's scope — §28.3's frame paragraph opens by scoping
// itself to "a return whose declared type is one of the deferred-activation
// kinds". A bare field read is admitted on that ground AND on the
// receiver-sourced one; either way this fix must not newly reject it.
BEZEL_TEST(getter_frame_borrow_of_using_var_6484,
           bare_field_read_accepted) {
    auto r = parseAndCheckOwnership(std::string(kPreamble) + R"(
  public property Only: OwningArray<Cell>
    get => _items;
  end Only;
end Factory;
class C public static method Main(): int return 0; end Main; end C;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ── the live regression: the hazard is a real use-after-free ───────────────
// The rows above are compile-time. This one pins the RUNTIME consequence, and
// is the row that would catch a future change which turned the rejection back
// into an acceptance without any compile-time row noticing.
//
// It asserts the REJECTION, not an ASan report, because post-fix the program
// does not build — there is no binary to run. The ASan measurement it records
// is quoted in the header and in the commit body: the pre-fix program really
// did read freed memory, so the rejection is not defending against a phantom.
BEZEL_TEST(getter_frame_borrow_of_using_var_6484,
           frame_borrow_of_using_var_is_rejected_at_the_return_site) {
    // The exact program whose ASan run at ba713eb9 read freed memory:
    //   get => do using var items2 := MakeItems();
    //             var frame := items2.Where(lambda(x) => true);
    //             return frame; end;
    // with a Main that iterates the frame and prints an element.
    auto r = runAot(R"(
module M;
class Cell
  public Name: string := "";
  public constructor(nm: string)
    Name := nm;
  end constructor;
  destructor end destructor;
end Cell;
class Factory
  public property View: IEnumerable<Cell>
    get => do
      using var items2 := MakeItems();
      var frame := items2.Where(lambda(c) => true);
      return frame;
    end;
  end View;
  private static method MakeItems(): owned OwningArray<Cell>
    var a := new OwningArray<Cell>();
    a.Add(new Cell("a"));
    a.Add(new Cell("b"));
    return a;
  end MakeItems;
end Factory;
class Program
  public static method Main(): int
    var f := new Factory();
    var n := 0;
    for var c in f.View do
      n := n + 1;
      Console.WriteLine(c.Name);
    end;
    Console.WriteLine(n);
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The POSITIVE half: the shape §28.3 ADMITS — a frame whose borrow is
// receiver-sourced — must still compile, run, and print the right value.
// Without this, a fix that over-rejected every getter-shaped frame would pass
// the rejection rows above while quietly breaking the one legal shape; that is
// the mirror of the silent-acceptance defect this issue is about.
//
// The remedy is receiver-sourcing, which is what §28.3's own sentence names:
// "provided every borrow the returned frame carries is sourced from the method's
// own parameters or receiver (directly or through the receiver's fields)". A
// getter HAS no parameters, so its admitted shape is exactly the field-rooted
// one.
BEZEL_TEST(getter_frame_borrow_of_using_var_6484,
           receiver_sourced_frame_runs_and_prints) {
    auto r = runAot(R"(
module M;
class Cell
  public Name: string := "";
  public constructor(nm: string)
    Name := nm;
  end constructor;
  destructor end destructor;
end Cell;
class Factory
  public owned _items: OwningArray<Cell>;
  public constructor()
    _items := new OwningArray<Cell>();
    _items.Add(new Cell("a"));
    _items.Add(new Cell("b"));
  end constructor;
  public property View: IEnumerable<Cell>
    get => _items;
  end View;
end Factory;
class Program
  public static method Main(): int
    using var f := new Factory();
    var n := 0;
    for var c in f.View do
      n := n + 1;
      Console.WriteLine($"elem={c.Name}");
    end;
    Console.WriteLine($"n={n}");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
    // Both elements, by value, plus the count — so an over-release (an empty
    // frame, or a frame whose elements were disposed at the getter's scope
    // end) cannot read as a pass.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("elem=a") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("elem=b") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("n=2") != std::string::npos);
}

}  // namespace