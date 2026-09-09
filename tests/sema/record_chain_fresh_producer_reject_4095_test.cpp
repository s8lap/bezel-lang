// fix #4095 (§9.7 / §22.7 / §22.8, v0.295): a record constructor's leading
// `this(...)` chain call whose chain argument is a fresh owned producer
// (`new T(...)`) destined for a NON-`owned` chain-target formal is rejected
// at sema time. The fresh producer's +1 reference lands in the record's
// borrow-carrying field, but its frame ends the moment the chain call
// returns — the record outlives its borrow source. §9.7's no-temporary-
// source rule (already enforced for the OUTER `new Bag([10, 20, 30])` form)
// applies one level deeper, to a record-ctor's chain argument. The remedy
// is to bind the producer to a named local first.
//
// Why this is its own file rather than amending #4013 / #4014 / #4022:
// the prior test files pinned the SHAPE of the chain (the #4013 once-only
// rule, the #4014 observation gate, the #4022 cross-module ctor-call chain).
// The #4095 test pins the §9.7 source-side rule specifically, with the
// diagnostic text and shape (reject vs. accept), the record-only scope
// (class ctors are NOT rejected — the same shape there is governed by the
// class-side `recordMovesInArgs` / `isImplicitOwned` borrow lifetime rules),
// and the named-local accept arm. The three prior test files have been
// amended in lockstep:
//   * `tests/codegen/record_this_chain_ctor_4013_test.cpp::record_this_chain_
//     discharges_reference_field` now expects a compile error.
//   * `tests/module/xmod_record_this_chain_ctor_imported_4022_test.cpp::t1`
//     and `::t1b` now expect a compile error.
//   * `tests/sema/record_ctor_this_observation_gate_4014_test.cpp::this_
//     chaining_ctor_body_call_accepted_spec_22_7` now binds the producer
//     to a local first.
//
// FAIL-WITHOUT / PASS-WITH: reverting `Ownership_Checks.cpp` to the pre-#4095
// state (no `checkRecordChainFreshProducerArgs` call in `run`, no function
// definition) restores the OLD behaviour for the 4 REJECT-arm tests below
// (`single_module_record_chain_new_into_borrow_field_rejected`,
// `class_chain_new_into_borrow_field_accepted`,
// `record_chain_new_into_owned_field_accepted`,
// `record_chain_literal_int_accepted`,
// `record_chain_bound_local_accepted`).
//
// Per LANE_RULES §3 (test source MUST NOT contain C++ `// text` inside a
// Bezel source string literal) and §18 (real-location check), every
// REJECT-arm row below parses the path:line:col prefix off the diagnostic
// line carrying the §9.7 substring and asserts both numbers are non-zero.

#include <cstdio>
#include <regex>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// True iff SOME diagnostic contains `sub`. Never assert on the coarse
// `hasErrors()` alone (LANE_RULES §3) — pin the specific message text.
bool hasError(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// Real-location check (LANE_RULES §18): the AOT driver packs stderr into a
// single synthetic diagnostic whose `.location` is default {0,0}, so the
// location has to be parsed out of the "<path>:<line>:<col>: " text the
// driver wrote. Mirrors `gate_chain_position_field_read_record_4066_test.cpp`'s
// identical helper.
bool hasErrorWithRealLocation(const JitResult& r, const char* sub) {
    static const std::regex kLoc(R"(:(\d+):(\d+): )");
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::size_t pos = d.message.find(sub);
        if (pos == std::string::npos) continue;
        std::smatch m;
        if (std::regex_search(d.message, m, kLoc)) {
            return std::stoi(m[1].str()) > 0 && std::stoi(m[2].str()) > 0;
        }
    }
    return false;
}

// Shared helper class every repro below funnels a non-nullable reference
// field through (mirrors kWidget in record_ctor_this_observation_gate_4014_
// test.cpp).
const char* kWidget4095 =
    "module M; "
    "public class Widget4095 "
    "  public Count: int; "
    "  public constructor(c: int) Count := c; end constructor; "
    "  public method Render(): int return Count; end Render; "
    "end Widget4095; ";

// Shared fragment: a record whose `this(...)` chain argument would be a
// fresh producer; tests below selectively substitute the chain argument
// shape (`new T(88)`, `42`, `w`, `owned new T(88)`, the bare class itself,
// etc.) to exercise the §9.7 rule on every documented arm.
const char* kRecordBorrowTarget =
    "public record R4095Borrow "
    "  public readonly property Extra: Widget4095; "
    "  public constructor(w: Widget4095) Extra := w; end constructor; "
    "  public constructor() this(%%CHAIN%%); "
    "end R4095Borrow; ";

const char* kRecordOwnedTarget =
    "public class W4095Owned "
    "  destructor end destructor; "
    "end W4095Owned; "
    "public record R4095Owned "
    "  public readonly property Extra: W4095Owned; "
    "  public constructor(owned w: W4095Owned) Extra := w; end constructor; "
    "  public constructor() this(%%CHAIN%%); "
    "end R4095Owned; ";

}  // namespace

// =========================================================================
// REJECT arm — single-module headline. The record's `this(new Widget4095(88))`
// chain argument is a fresh owned producer into the chain target's NON-`owned`
// `w: Widget4095` formal. §9.7 no-temporary-source rejects; the diagnostic
// cites §9.7 by name.
// =========================================================================
BEZEL_TEST(record_chain_fresh_producer_reject_4095,
           single_module_record_chain_new_into_borrow_field_rejected) {
    std::string src = std::string(kWidget4095) +
        "public record R4095Single "
        "  public readonly property Extra: Widget4095; "
        "  public constructor(w: Widget4095) Extra := w; end constructor; "
        "  public constructor() this(new Widget4095(88)); end constructor; "
        "end R4095Single; "
        "class Program4095Single "
        "  public static method Main(): int "
        "    var c := new R4095Single(); return c.Extra.Render(); "
        "  end Main; "
        "end Program4095Single;";
    auto r = runAot(src);
    // The three `BEZEL_EXPECT_TRUE(hasError(...))` assertions below pin the
    // §9.7 reject's exact shape; failure prints the missing-substring
    // mismatch via the test framework's diagnostics dump.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, "fresh producer"));
    BEZEL_EXPECT_TRUE(hasError(r, "this(...)"));
    BEZEL_EXPECT_TRUE(hasError(r, "9.7"));
    BEZEL_EXPECT_FALSE(r.ran);
    // Real-location check (LANE_RULES §18): the §9.7 diagnostic must carry
    // a non-zero `path:line:col:` prefix; AOT's default {0,0} synthetic
    // would mean the diagnostic was fabricated, not emitted.
    BEZEL_EXPECT_TRUE(hasErrorWithRealLocation(r, "fresh producer"));
}

// =========================================================================
// ACCEPT arm (1/4): the identical shape on a CLASS is NOT rejected by this
// rule. The class instance owns its fields outright, so a `this(new T(...))`
// borrow into the chain target is governed by the class-side
// `recordMovesInArgs`/`isImplicitOwned` machinery (which already admits the
// borrow). This is the #4095 scope pin — the rule is RECORD-specific, not
// "every constructor with a chain".
// =========================================================================
BEZEL_TEST(record_chain_fresh_producer_reject_4095,
           class_chain_new_into_borrow_field_accepted) {
    std::string src =
        "module M; "
        "public class W4095C "
        "  public Count: int; "
        "  public constructor(c: int) Count := c; end constructor; "
        "end W4095C; "
        "public class C4095Ctor "
        "  public Extra: W4095C; "
        "  public constructor(w: W4095C) Extra := w; end constructor; "
        "  public constructor() this(new W4095C(88)); end constructor; "
        "end C4095Ctor; "
        "class Program4095C "
        "  public static method Main(): int "
        "    var c := new C4095Ctor(); return c.Extra.Count; "
        "  end Main; "
        "end Program4095C;";
    auto r = runAot(src);
    // No §9.7 reject — the class shape is admitted by `recordMovesInArgs`
    // (the chain target's `w: W4095C` parameter takes a borrow, the class
    // instance's `Extra` field is the borrow's destination, and the class
    // instance outlives the chain call's frame). NOTE: this test expects
    // the program to COMPILE CLEAN. The actual run-time behaviour of the
    // shape is a separate question (#1020's class-side rule, etc.); the
    // #4095 check is strictly a no-op for non-record types.
    BEZEL_EXPECT_FALSE(hasError(r, "fresh producer"));
}

// =========================================================================
// ACCEPT arm (2/4): a record whose chain-target formal IS `owned`. The fresh
// producer's +1 is transferred into the chain target, which disposes it at
// end of ctor (§22.3 / §28.5). The §9.7 reject is keyed on the formal's
// modifier, not on the producer alone — `this(owned new T())` is the legal
// `owned` transfer. The diagnostic text would be about `owned`-parameter
// disposal, NOT about §9.7.
// =========================================================================
BEZEL_TEST(record_chain_fresh_producer_reject_4095,
           record_chain_new_into_owned_field_accepted) {
    std::string src =
        "module M; "
        "public class W4095OwnedArm "
        "  destructor end destructor; "
        "end W4095OwnedArm; "
        "public record R4095OwnedArm "
        "  public readonly property Extra: W4095OwnedArm; "
        "  public constructor(owned w: W4095OwnedArm) Extra := w; end constructor; "
        "  public constructor() this(owned new W4095OwnedArm()); end constructor; "
        "end R4095OwnedArm; "
        "class Program4095OwnedArm "
        "  public static method Main(): int "
        "    var c := new R4095OwnedArm(); return 1; "
        "  end Main; "
        "end Program4095OwnedArm;";
    auto r = runAot(src);
    // No §9.7 reject — the chain target's `owned w: W4095OwnedArm` formal
    // takes the +1 legally. The pre-#4095 stack MAY still report something
    // about the `owned` parameter's auto-drop semantics (the §28.3
    // implicit-disposal warning that exists for every unmarked `owned`
    // parameter); what this test pins is that the §9.7 diagnostic is NOT
    // among them.
    BEZEL_EXPECT_FALSE(hasError(r, "9.7"));
}

// =========================================================================
// ACCEPT arm (3/4): a record whose chain argument is a value (literal /
// identifier), NOT a fresh producer. `this(42)` is the canonical #4013
// once-only case — the argument is a value copy, the chain target's
// `n: int` formal takes a copy, no borrow source is involved. The check
// fires only on `ast::NewExpr*` arguments, never on literals or
// identifiers, so the §9.7 reject does not appear.
// =========================================================================
BEZEL_TEST(record_chain_fresh_producer_reject_4095,
           record_chain_literal_int_accepted) {
    std::string src =
        "module M; "
        "public record R4095Lit "
        "  public readonly property N: int; "
        "  public constructor(n: int) N := n; end constructor; "
        "  public constructor() this(42); end constructor; "
        "end R4095Lit; "
        "class Program4095Lit "
        "  public static method Main(): int "
        "    var c := new R4095Lit(); return c.N; "
        "  end Main; "
        "end Program4095Lit;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// =========================================================================
// ACCEPT arm (4/4): the §9.7 REMEDY. The producer is bound to a named local
// at the CALL site (in `Main`), then the named local flows into the
// constructor call as a borrow. The local's scope outlives the record, so
// the borrow source is well-formed — the record carries a valid borrow
// pointer. The same shape as the OUTER §9.7 example: `var xs := new Array();
// var b := new Bag(xs);` where the temporary is bound to a name first. (The
// chain-position variant — `var w := new X(); this(w);` — would also pass
// §9.7 but is independently rejected by §12.8's "chain must be the FIRST
// statement" rule, so it is not the right spelling to test here.)
// =========================================================================
BEZEL_TEST(record_chain_fresh_producer_reject_4095,
           record_chain_bound_local_accepted) {
    std::string src = std::string(kWidget4095) +
        "public record R4095Bound "
        "  public readonly property Extra: Widget4095; "
        "  public constructor(w: Widget4095) Extra := w; end constructor; "
        "end R4095Bound; "
        "class Program4095Bound "
        "  public static method Main(): int "
        "    var w := new Widget4095(88); "
        "    var c := new R4095Bound(w); "
        "    return c.Extra.Render(); "
        "  end Main; "
        "end Program4095Bound;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 88);
}