// Regression tests for #7893 -- a read through a PINNED ALIAS after a
// DOMINATING explicit disposal was accepted, and the program then read freed
// memory under ASan.
//
//   var a := new R(1);
//   var b := a;            // b is an ALIAS of a
//   using var p := b;      // p PINS b (§28.10)
//   a.Dispose();           // the disposal DOMINATES the later read
//   var s := p.Use();      // a READ THROUGH THE PIN -- must be a compile error
//
// Language spec §28.2 (v0.355) "Use after a dominating explicit disposal":
//   "A use of a local variable -- a read, a borrow taken from it, a member
//    access, an argument position, a consume -- after an explicit `Dispose()`
//    or direct `DisposeAsync()` call ... on the same variable that dominates
//    the use site on every path is a **compile error** ... The same error
//    reaches a use through a local borrow of the variable's value: where a
//    local holds a borrow of that value -- an alias (`var b := a;`,
//    `const b := a;`), a `using var` pin (§28.10) ... a use of the holder --
//    a read or member access through it, passing it, re-borrowing it,
//    invoking the closure, enumerating or disposing the frame -- is the same
//    compile error; the diagnostic names the use, the holder, the disposed
//    source and the disposing site."
// and §28.10 (v0.350): "an explicit `Dispose()` of the source is permitted; a
// read through the pinned binding that the disposal dominates is §28.2's
// compile error, and one the disposal reaches on only some paths raises
// `DisposedAccessException` in checked disposal mode".
//
// Root cause: `checkRead`'s direct arm reads the DISPOSED BINDING's own state
// (`state == Disposed`), so it never saw the HOLDER shape: the pin's own
// state is Owned (a borrow-pin rides the adoption machinery, §28.10) and a
// plain alias's state is Borrowed. Nothing asked whether the value the
// holder's borrow reaches had been disposed.
//
// SCOPE OF THE RULE, and the two shapes it deliberately leaves to §28.2's
// RUNTIME backstop (each pinned by a row below):
//   * the PIN over a plain local read (`using var p := b;`) is the shape
//     §28.10's own sentence names, and the shape this issue reports;
//   * a pin over a MEMBER read or a CALL (`using var view := h.Borrowed();`)
//     borrows a value that is not the disposed binding -- §28.2's clause is
//     "on the same variable" -- so the runtime backstop stands behind the
//     use (the spec's own rule corpus, case `s28_10_0012`, expects that
//     program to COMPILE and to fault at runtime in checked disposal mode);
//   * a plain ALIAS holder (`var b := a;`) is the other §28.2 list entry, but
//     the spec's own rule corpus admits that program too (cases
//     `s28_2_0011` / `s28_2_0012`: compile, then fault at runtime in checked
//     mode / be undefined behaviour unchecked), so it stays with the
//     runtime backstop as well.
//
// The rows are COMPILE-TIME only (`parseAndCheckOwnership`): the defect is a
// compile-time over-acceptance, and the direct-shape control is a WARNING
// (the §28.2 v0.131 severity ruling, unchanged) whose runtime behaviour
// (a `DisposedAccessException` in checked mode) is not what this file pins.

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// The new diagnostic's stable fragments (§28.2 / §28.10).
constexpr const char* kHolderUse =
    "after a dominating explicit Dispose() of";

// The §28.2 direct-shape warning -- its severity must NOT move.
constexpr const char* kDirectWarning =
    "use of `a` after a dominating explicit Dispose() call";

bool hasError(const OwnershipResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

bool hasWarning(const OwnershipResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

const std::string kHead =
    "module Probe;\n"
    "public sealed class R\n"
    "  public F: int := 7;\n"
    "  private _id: int;\n"
    "  public constructor(id: int)\n"
    "    _id := id;\n"
    "  end constructor;\n"
    "  public method Use(): int => _id;\n"
    "end R;\n"
    "public static class Sink\n"
    "  public static method Take(r: R): int => 1;\n"
    "end Sink;\n"
    "public class Program\n"
    "  public static method Body(k: int)\n";

const std::string kTail =
    "  end Body;\n"
    "  public static method Main(): int\n"
    "    Body(0);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

std::string prog(const std::string& body) { return kHead + body + kTail; }

}  // namespace

// REJECT: the issue's own shape -- a read through the pin after a dominating
// explicit disposal.
BEZEL_TEST(ownership_pin_use_after_dominating_dispose_7893,
           read_through_pin_after_dispose_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var a := new R(1);\n"
        "    var b := a;\n"
        "    using var p := b;\n"
        "    a.Dispose();\n"
        "    var s := p.Use();\n"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
    BEZEL_EXPECT_TRUE(hasError(r, "use of `p`"));
    BEZEL_EXPECT_TRUE(hasError(r, "of `a`"));
}

// REJECT: the `!` spelling -- §17.8 classifies `x!` exactly as the bare `x`,
// so the pin holds the same borrow.
BEZEL_TEST(ownership_pin_use_after_dominating_dispose_7893,
           read_through_bang_pin_after_dispose_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var a := new R(1);\n"
        "    var b := a;\n"
        "    using var p := b!;\n"
        "    a.Dispose();\n"
        "    var s := p.Use();\n"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
}

// REJECT: the MEMBER-ACCESS shape -- §28.2 lists "a read or member access
// through it".
BEZEL_TEST(ownership_pin_use_after_dominating_dispose_7893,
           member_access_through_pin_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var a := new R(1);\n"
        "    var b := a;\n"
        "    using var p := b;\n"
        "    a.Dispose();\n"
        "    var s := p._id;\n"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
}

// REJECT: the ARGUMENT-POSITION shape -- §28.2 lists "passing it".
BEZEL_TEST(ownership_pin_use_after_dominating_dispose_7893,
           argument_through_pin_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var a := new R(1);\n"
        "    var b := a;\n"
        "    using var p := b;\n"
        "    a.Dispose();\n"
        "    var s := Sink.Take(p);\n"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
}

// REJECT: the RE-BORROWING shape -- §28.2 lists "re-borrowing it".
BEZEL_TEST(ownership_pin_use_after_dominating_dispose_7893,
           reborrow_through_pin_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var a := new R(1);\n"
        "    var b := a;\n"
        "    using var p := b;\n"
        "    a.Dispose();\n"
        "    var q := p;\n"
        "    var s := q.Use();\n"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
}

// REJECT: the disposal DOMINATES through a branch join -- both arms dispose,
// so the kill reaches the use on every path (§28.2's "dominates the use site
// on every path"), while a one-armed disposal does NOT.
BEZEL_TEST(ownership_pin_use_after_dominating_dispose_7893,
           dominating_disposal_through_a_branch_join_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(
        "module Probe;\n"
        "public sealed class R\n"
        "  private _id: int;\n"
        "  public constructor(id: int)\n"
        "    _id := id;\n"
        "  end constructor;\n"
        "  public method Use(): int => _id;\n"
        "end R;\n"
        "public class Program\n"
        "  public static method Body(k: int, c: bool)\n"
        "    var a := new R(1);\n"
        "    var b := a;\n"
        "    using var p := b;\n"
        "    if c then\n"
        "      a.Dispose();\n"
        "    else\n"
        "      a.Dispose();\n"
        "    end if;\n"
        "    var s := p.Use();\n"
        "  end Body;\n"
        "  public static method Main(): int\n"
        "    Body(0, true);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
}

// REJECT: the `DisposeAsync()` spelling -- §28.2 names "an explicit
// `Dispose()` or direct `DisposeAsync()` call", and §28.12's async form
// discharges the value the same way.
BEZEL_TEST(ownership_pin_use_after_dominating_dispose_7893,
           dispose_async_through_pin_rejected_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(
        "module Probe; import System;\n"
        "public sealed class R: IAsyncDisposable\n"
        "  private _id: int;\n"
        "  public constructor(id: int)\n"
        "    _id := id;\n"
        "  end constructor;\n"
        "  public method Use(): int => _id;\n"
        "  public async method DisposeAsync(): Task\n"
        "  end DisposeAsync;\n"
        "end R;\n"
        "public class Program\n"
        "  public static async method Body(k: int): Task\n"
        "    var a := new R(1);\n"
        "    var b := a;\n"
        "    using var p := b;\n"
        "    await a.DisposeAsync();\n"
        "    var s := p.Use();\n"
        "  end Body;\n"
        "  public static method Main(): int\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kHolderUse));
}

// ACCEPT: the SAME shape with NO disposal -- the pin is a legal borrow.
BEZEL_TEST(ownership_pin_use_after_dominating_dispose_7893,
           read_through_pin_without_disposal_accepted_spec_28_10) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var a := new R(1);\n"
        "    var b := a;\n"
        "    using var p := b;\n"
        "    var s := p.Use();\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasError(r, kHolderUse));
}

// ACCEPT: the disposal on ONE path only -- §28.2: "A holder whose source was
// disposed on only some paths to the use ... is not rejected: a use through
// it raises `DisposedAccessException` in checked disposal mode and is
// undefined behavior unchecked, the runtime backstop above."
BEZEL_TEST(ownership_pin_use_after_dominating_dispose_7893,
           non_dominating_disposal_accepted_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(
        "module Probe;\n"
        "public sealed class R\n"
        "  private _id: int;\n"
        "  public constructor(id: int)\n"
        "    _id := id;\n"
        "  end constructor;\n"
        "  public method Use(): int => _id;\n"
        "end R;\n"
        "public class Program\n"
        "  public static method Body(k: int, c: bool)\n"
        "    var a := new R(1);\n"
        "    var b := a;\n"
        "    using var p := b;\n"
        "    if c then\n"
        "      a.Dispose();\n"
        "    end if;\n"
        "    var s := p.Use();\n"
        "  end Body;\n"
        "  public static method Main(): int\n"
        "    Body(0, false);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasError(r, kHolderUse));
}

// ACCEPT: a pin over a MEMBER read, with the disposal on the HOLDER -- the
// value the pin borrows is the field the call returns, not the disposed
// binding, so §28.2's "on the same variable" clause is not met and the
// runtime backstop stands behind the use. This is the spec's own rule-corpus
// case `s28_10_0012` ("an explicit `Dispose()` of a borrow-pin's source is
// permitted, and a read through the pinned binding afterwards raises
// `DisposedAccessException` in checked disposal mode").
BEZEL_TEST(ownership_pin_use_after_dominating_dispose_7893,
           pin_over_a_member_read_left_to_the_runtime_backstop_spec_28_10) {
    OwnershipResult r = parseAndCheckOwnership(
        "module Probe;\n"
        "public class Widget\n"
        "  public property Label: string;\n"
        "  public constructor()\n"
        "    Label := \"live\";\n"
        "  end constructor;\n"
        "  destructor\n"
        "  end destructor;\n"
        "end Widget;\n"
        "public class Holder\n"
        "  private owned _w: Widget;\n"
        "  public constructor()\n"
        "    _w := new Widget();\n"
        "  end constructor;\n"
        "  public method Borrowed(): Widget => _w;\n"
        "end Holder;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var h := new Holder();\n"
        "    using var view := h.Borrowed();\n"
        "    h.Dispose();\n"
        "    var s := view.Label;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasError(r, kHolderUse));
}

// ACCEPT: the plain-ALIAS holder -- §28.2's list names it first ("an alias
// (`var b := a;`, `const b := a;`)"), but the spec's own rule corpus admits
// that program and expects the RUNTIME backstop to fault it (cases
// `s28_2_0011` -- "the default build is checked: a read through a surviving
// borrow after Dispose faults" -- and `s28_2_0012`, the same program under
// `--unchecked-disposal`, which the corpus expects to COMPILE). So the alias
// holder is left to §28.2's checked-mode backstop, not to a compile error.
BEZEL_TEST(ownership_pin_use_after_dominating_dispose_7893,
           plain_alias_left_to_the_runtime_backstop_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var a := new R(1);\n"
        "    var b := a;\n"
        "    a.Dispose();\n"
        "    var s := b.Use();\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasError(r, kHolderUse));
}

// CONTROL: the DIRECT shape keeps its own diagnostic AND its own severity --
// the §28.2 v0.131 WARNING ruling is unchanged by this fix.
BEZEL_TEST(ownership_pin_use_after_dominating_dispose_7893,
           direct_use_after_dispose_keeps_its_warning_spec_28_2) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var a := new R(1);\n"
        "    a.Dispose();\n"
        "    var s := a.Use();\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasWarning(r, kDirectWarning));
    BEZEL_EXPECT_FALSE(hasError(r, kDirectWarning));
}
