// Ownership tests for §28.14 (v0.180, #1439) "scoped construction" — a
// class/interface-typed `scoped` parameter, generalizing the function-typed
// storage-bounded capture of §28.14 (v0.175) to a passive BORROW. Checker
// fallout tracked as #1439's follow-up (#1545).
//
// §28.14: "Class- and interface-typed `scoped` parameters: scoped
// construction (v0.180, #1439 — owner-accepted)... a class- or interface-
// typed parameter may be declared `scoped` — `constructor(scoped stream:
// Stream)` — applying the same two-sided contract to a passive borrow
// instead of a callback:
//  - Callee side: the callee may store the borrow in a direct non-owned
//    field of the bounding object... may not return it or store it anywhere
//    else.
//  - Caller side: the argument carries the call-site marker...  and is
//    admitted under the bounding-object forms above with the argument's
//    borrow source in place of a closure's capture set: the source must
//    satisfy the same declared-before/self/`this`-derived tests, and it is
//    frozen until the bound's scope exits. A fresh producer argument is
//    rejected (§9.7's no-temporary-source rule)."
//
// §22.3: "a class- or interface-typed `scoped` argument is written
// `M(scoped x)`, paralleling `owned` above."
//
// The four first conformance tests recorded by the #1439 RFC: (a) a fresh
// producer is a compile error; (b) a named local declared AFTER the reader
// is a compile error; (c) declared BEFORE, source frozen, compiles; (d)
// disposing/moving the frozen source before scope exit is rejected. The
// remaining tests cover the checker-fallout pieces #1545 called out:
// relaxing `checkScopedIsFunctionTyped` to admit class/interface types (and
// confirming record/other types remain excluded), and the callee-side
// storage rule (plain field of `this` admitted; return / other-object
// storage rejected; forwarding to `this` admitted; reassignment rejected).

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

bool diagContains(const bezel::test::OwnershipResult& r,
                  const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// Source (the borrowed resource) + Reader (a user callee with a `scoped`
// class-typed constructor parameter stored in an ORDINARY non-owned field —
// the §28.14 v0.180 "scoped construction" storage shape, no `scoped` field
// marker). `body` is spliced in as Prog's Main body.
std::string prog(const std::string& body) {
    return std::string(
        "module M; "
        "class Source "
        "  public method Ping() end Ping; "
        "  destructor end; "
        "end Source; "
        "class Reader "
        "  private src: Source; "
        "  public constructor(scoped src: Source) this.src := src; end; "
        "  public method Touch() this.src.Ping(); end Touch; "
        "  destructor end; "
        "end Reader; "
        "class Prog "
        "  public static method Sink(owned x: Source) x.Dispose(); end Sink; "
        "  public static method Main(): int ") + body +
        "  end Main; "
        "end Prog;";
}

}  // namespace

// ===========================================================================
// Call-site admission (#1439 RFC conformance tests a-d)
// ===========================================================================

// (a) §9.7 / §28.14 v0.180: a FRESH PRODUCER argument — an unbound owned
// temporary — is rejected. `new StreamReader(scoped new FileStream(...))` in
// the spec's own example; here `new Reader(scoped new Source())`.
BEZEL_TEST(scoped_construction_1545, fresh_producer_rejected_spec_9_7) {
    auto r = parseAndCheckOwnership(prog(
        "    using var reader := new Reader(scoped new Source()); "
        "    reader.Touch(); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "fresh producer"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.14"));
}

// (b) a named-local source declared AFTER the reader is a compile error —
// ordinary name resolution rejects the forward reference (locals are not
// forward-visible; §28.14's admission never even runs).
BEZEL_TEST(scoped_construction_1545, source_declared_after_reader_rejected) {
    auto r = parseAndCheckOwnership(prog(
        "    using var reader := new Reader(scoped src); "
        "    using var src := new Source(); "
        "    reader.Touch(); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "unresolved identifier"));
}

// (c) a named-local source declared BEFORE the reader is admitted — the
// whole spec shape (constructor form 1 bounding, declared-before source)
// compiles cleanly.
BEZEL_TEST(scoped_construction_1545, source_declared_before_reader_admitted) {
    auto r = parseAndCheckOwnership(prog(
        "    using var src := new Source(); "
        "    using var reader := new Reader(scoped src); "
        "    reader.Touch(); "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (d) FREEZE: the source may not be DISPOSED before the bounding reader's
// scope exits (disposal counts as "moved from" for the freeze, §28.14
// v0.180's admission list item 2c).
BEZEL_TEST(scoped_construction_1545,
          freeze_dispose_of_source_before_scope_exit_rejected) {
    auto r = parseAndCheckOwnership(prog(
        "    var src := new Source(); "
        "    using var reader := new Reader(scoped src); "
        "    src.Dispose(); "
        "    reader.Touch(); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "frozen"));
}

// (d) FREEZE: the source may not be MOVED (transferred) before the bounding
// reader's scope exits.
BEZEL_TEST(scoped_construction_1545,
          freeze_move_of_source_before_scope_exit_rejected) {
    auto r = parseAndCheckOwnership(prog(
        "    var src := new Source(); "
        "    using var reader := new Reader(scoped src); "
        "    Prog.Sink(owned src); "
        "    reader.Touch(); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "frozen"));
}

// ===========================================================================
// Call-site admission — no-over-rejection controls
// ===========================================================================

// §28.3 / §28.14: a source that is itself a PLAIN (non-owned) borrow
// PARAMETER of the enclosing method is admitted with NO freeze needed — a
// parameter precedes every local, and §28.3's borrowed-field rule already
// admits a parameter-sourced borrow into a non-owned field; `scoped` upgrades
// that residual trust without requiring the parameter to be re-bound to a
// local first.
BEZEL_TEST(scoped_construction_1545, forwarded_enclosing_parameter_admitted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Source "
        "  public method Ping() end Ping; "
        "  destructor end; "
        "end Source; "
        "class Reader "
        "  private src: Source; "
        "  public constructor(scoped src: Source) this.src := src; end; "
        "  public method Touch() this.src.Ping(); end Touch; "
        "  destructor end; "
        "end Reader; "
        "class Attacher "
        "  public static method Attach(src: Source): int "
        "    using var reader := new Reader(scoped src); "
        "    reader.Touch(); "
        "    return 0; "
        "  end Attach; "
        "end Attacher; "
        "class Prog "
        "  public static method Main(): int "
        "    using var s := new Source(); "
        "    return Attacher.Attach(s); "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14: a bounding object that is NOT a `using var` local (a plain owned
// local receiver) admits no borrow argument — mirrors the function-typed
// `non_using_var_bounding_rejects_captures` control.
BEZEL_TEST(scoped_construction_1545, non_using_var_bounding_rejects_borrow) {
    auto r = parseAndCheckOwnership(prog(
        "    using var src := new Source(); "
        "    var reader := new Reader(scoped src); "
        "    reader.Dispose(); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ===========================================================================
// Type admission (checkScopedIsFunctionTyped relaxation)
// ===========================================================================

// §28.14 v0.180: an INTERFACE-typed `scoped` parameter is admitted too (not
// class-only) — the borrow source's static type is the interface.
BEZEL_TEST(scoped_construction_1545, interface_typed_scoped_param_admitted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "public interface ISource "
        "  method Ping(); "
        "end ISource; "
        "class Source : ISource "
        "  public method Ping() end Ping; "
        "  destructor end; "
        "end Source; "
        "class Reader "
        "  private src: ISource; "
        "  public constructor(scoped src: ISource) this.src := src; end; "
        "  public method Touch() this.src.Ping(); end Touch; "
        "  destructor end; "
        "end Reader; "
        "class Prog "
        "  public static method Main(): int "
        "    using var s := new Source(); "
        "    using var r := new Reader(scoped s); "
        "    r.Touch(); "
        "    return 0; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14 v0.180's admission is class/interface only — a RECORD-typed `scoped`
// parameter remains rejected (records are a distinct TypeDeclKind from the
// class/interface pair the relaxation admits).
BEZEL_TEST(scoped_construction_1545, record_typed_scoped_param_still_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "public record Point(X: int, Y: int); "
        "class Reader "
        "  public method M(scoped p: Point) end M; "
        "end Reader;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "class-typed, or interface-typed"));
}

// Control: a primitive-typed `scoped` parameter is still rejected (unchanged
// from v0.175 — spec §22.3's original restriction).
BEZEL_TEST(scoped_construction_1545, primitive_typed_scoped_param_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public method M(scoped x: int) end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "function-typed"));
}

// ===========================================================================
// Callee verification (storage shape: a PLAIN field of `this`, not a
// `scoped` field — "No new field kind is introduced")
// ===========================================================================

// §28.14 v0.180: the scoped borrow parameter may not be RETURNED.
BEZEL_TEST(scoped_construction_1545, callee_return_of_scoped_borrow_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Source "
        "  public method Ping() end Ping; "
        "  destructor end; "
        "end Source; "
        "class Leaker "
        "  public method Leak(scoped s: Source): Source "
        "    return s; "
        "  end Leak; "
        "end Leaker;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "returned"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.14"));
}

// §28.14 v0.180: the scoped borrow parameter may be stored only into a plain
// field of the bounding object `this` ITSELF — storing it into another
// object's field is rejected (mirrors the function-typed "store into ANOTHER
// object" rejection).
BEZEL_TEST(scoped_construction_1545,
          callee_store_into_other_object_field_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Source "
        "  public method Ping() end Ping; "
        "  destructor end; "
        "end Source; "
        "class Other "
        "  public src: Source?; "
        "  destructor end; "
        "end Other; "
        "class Leaker "
        "  public method Leak(scoped s: Source, o: Other) "
        "    o.src := s; "
        "  end Leak; "
        "end Leaker;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The store into another object's field is rejected: a `scoped` param may
    // only be stored into a `scoped` field of `this` (§28.14). Match the actual
    // diagnostic wording rather than a paraphrase.
    BEZEL_EXPECT_TRUE(diagContains(r, "scoped"));
}

// §28.14 v0.180: forwarding to a `scoped` parameter of a call on `this`
// (same bounding object) is admitted — both the explicit `this.` and the
// bare-name form. This is the admitted STORAGE path: `Inner` stores into its
// own plain field.
BEZEL_TEST(scoped_construction_1545, callee_forward_on_this_admitted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Source "
        "  public method Ping() end Ping; "
        "  destructor end; "
        "end Source; "
        "class Reader "
        "  private src: Source?; "
        "  public method Inner(scoped s: Source) this.src := s; end Inner; "
        "  public method OuterA(scoped s: Source) this.Inner(s); end OuterA; "
        "  public method OuterB(scoped s: Source) Inner(s); end OuterB; "
        "  destructor end; "
        "end Reader;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14: a scoped borrow parameter may not be REASSIGNED.
BEZEL_TEST(scoped_construction_1545, callee_reassign_scoped_borrow_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Source "
        "  public method Ping() end Ping; "
        "  destructor end; "
        "end Source; "
        "class Reader "
        "  public method M(scoped s: Source, other: Source) "
        "    s := other; "
        "  end M; "
        "end Reader;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "may not be reassigned"));
}
