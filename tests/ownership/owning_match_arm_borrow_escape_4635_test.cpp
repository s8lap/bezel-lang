// K8.4 at the §35.5 owning-match boundary — an arm result that BORROWS a
// payload the arm's own pattern bound may not escape the `case` construct
// (issue #4635; spec §28.3 K8.3/K8.4, §35.5 *The owning match*, §19.3, v0.307).
//
// THE RULE. §28.3 K8.3: a `case` scrutinee's boundary is "the whole construct
// — pattern bindings are borrows of the scrutinee, and the temporary survives
// arm selection, guard evaluation, and the selected arm's completion — in
// statement position exactly as in expression position". K8.4: "a temporary is
// an admissible borrow source only within its boundary; a borrow or frame that
// would escape it is rejected, never lifetime-extended — the language has no
// general lifetime-extension mechanism".
//
// §35.5 *The owning match* names the two ANNOUNCING scrutinee forms that make a
// match an owning one — an unbound owned temporary of owning-choice
// classification, and "a named owned binding under an explicit scrutinee
// transfer marker" (`case move o of …`) — and makes an `owned`-payload binding
// in such a match "an **owned local of the arm body's scope**". §28.3's arm-meet
// row then makes an UNMARKED named owned binding written as an arm a BORROW
// arm. So `W(var w, _) => w` is a borrow of a payload whose owner is gone when
// the construct completes: exactly K8.4's rejected shape.
//
// THE BUG (#4635). The checker admitted it. MEASURED on HEAD f644098f:
//
//   using var other := new Widget("other");
//   var o := Outer.W(new Widget("a"), new Widget("b"));
//   var got := (case move o of W(var w, _) => w, X => other);
//   Console.WriteLine($"got={got.Ping()}");   // reads payload `a` AFTER `end`
//
// compiled clean and printed `got=7` — a read through a borrow of a consumed
// value. It survived only because #4617's per-slot discharge marks a bound slot
// OUT of the construct-completion reclaim, so the payload leaked instead of
// being disposed. Give that binding its §28.3 auto-drop (the sibling
// unadopted-binding leak) and the same program is a use-after-free, which is
// why this hole had to close first.
//
// OBSERVABILITY. The pre-fix behaviour is a clean COMPILE, so the oracle is the
// SPECIFIC diagnostic text, never `failedAtCompileTimeWithRealLocation` (which
// would answer true for any unrelated rejection) and never `!r.ran` (runAot
// synthesizes an Error diagnostic for a runtime crash too — tests/README.md).
// Without the fix every REJECT row's `rejectsWith` returns false.
//
// per §28.3 K8.3/K8.4, §35.5, §19.3, v0.307

#include <string>

#include "support/aot_support.hpp"  // runAot — the strict AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The exact §35.5/K8.4 diagnostic this fix introduces. Asserting the specific
// text is load-bearing: the pre-fix compiler ACCEPTS these programs, so any
// weaker oracle (a bare "it failed", a real-location backstop) would not
// distinguish the fix from an unrelated error.
const char* kEscapeNeedle = "borrows owning-match payload binding";

bool hasErrorContaining(const JitResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

bool rejectsWith(const std::string& program, const char* needle) {
    auto r = runAot(program);
    return hasErrorContaining(r, needle);
}

// §35.5's own owning-choice shape: two `owned` class payloads and a
// payload-free case, so an arm can bind one slot and discard the other.
// `Self()` returns a plain (borrowing) reference to the receiver — the
// borrow-returning chain row (4) needs it.
const char* kPrelude =
    "module M; "
    "public class Widget "
    "  private _n: string; "
    "  public constructor(n: string) _n := n; end constructor; "
    "  destructor Console.WriteLine($\"dispose {_n}\"); end destructor; "
    "  public method Ping(): int => 7; "
    "  public method Self(): Widget return this; end Self; "
    "end Widget; "
    "public choice Outer W(owned a: Widget, owned b: Widget), X; end Outer; "
    "public class Program ";

std::string prog(const std::string& mainBody) {
    return std::string(kPrelude) +
           "  public static method Main(): int " + mainBody +
           "  end Main; "
           "end Program;";
}

// --- REJECT rows: the escaping borrow ------------------------------------

// (1) THE ISSUE'S REPRO, verbatim: §35.5's SECOND announcing scrutinee form
// (`case move o of`), an arm that binds payload `a` and returns it unmarked,
// and a read of the result AFTER the construct completes.
BEZEL_TEST(owning_match_arm_borrow_escape_4635,
           marked_scrutinee_arm_binding_escapes_rejected_28_3_k8_4) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    using var other := new Widget(\"other\"); "
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    var got := (case move o of "
        "      W(var w, _) => w, "
        "      X => other); "
        "    Console.WriteLine($\"got={got.Ping()}\"); "
        "    return 0; "), kEscapeNeedle));
}

// (2) The FIRST announcing scrutinee form — an unbound owned temporary of
// owning-choice classification (§35.3's case construction mints one). Same
// escape, same rejection: §35.5 states both forms of the owning match, and
// K8.3 gives both the same construct boundary.
BEZEL_TEST(owning_match_arm_borrow_escape_4635,
           unbound_temporary_arm_binding_escapes_rejected_35_5) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    using var other := new Widget(\"other\"); "
        "    var got := (case Outer.W(new Widget(\"a\"), new Widget(\"b\")) of "
        "      W(var w, _) => w, "
        "      X => other); "
        "    Console.WriteLine($\"got={got.Ping()}\"); "
        "    return 0; "), kEscapeNeedle));
}

// (3) The NAKED binding spelling (§19.3's `W(w, _)`), which binds the same
// slot through `PositionalPattern::nakedBinding` rather than a `BindingPattern`
// — the shape #4617's per-slot mark also has to recognize. Same rejection.
BEZEL_TEST(owning_match_arm_borrow_escape_4635,
           naked_binding_spelling_escapes_rejected_19_3) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    using var other := new Widget(\"other\"); "
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    var got := (case move o of "
        "      W(w, _) => w, "
        "      X => other); "
        "    Console.WriteLine($\"got={got.Ping()}\"); "
        "    return 0; "), kEscapeNeedle));
}

// (4) A BORROW-RETURNING chain rooted at the binding escapes exactly as the
// bare read does — K8.4 rejects "a borrow or frame" whose source is the
// boundary's temporary, not only a bare identifier arm.
BEZEL_TEST(owning_match_arm_borrow_escape_4635,
           borrow_chain_rooted_at_binding_escapes_rejected_28_3_k8_4) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "    using var other := new Widget(\"other\"); "
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    var got := (case move o of "
        "      W(var w, _) => w.Self(), "
        "      X => other); "
        "    Console.WriteLine($\"got={got.Ping()}\"); "
        "    return 0; "), kEscapeNeedle));
}

// --- ACCEPT controls: shapes the rule must NOT touch ----------------------

// (5) A BORROWING match over a named owned local read UNMARKED (§35.5: "the
// unmarked read is a borrow"). `o` is not consumed, it outlives the construct,
// and its payload binding is an ordinary borrow whose source is bounded by
// `o`'s own scope. This must keep compiling and running — the rejection is
// specific to the OWNING match, not to arm bindings in general.
BEZEL_TEST(owning_match_arm_borrow_escape_4635,
           borrowing_match_arm_binding_still_accepted_35_5) {
    auto r = runAot(prog(
        "    using var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    using var other := new Widget(\"other\"); "
        "    var got := (case o of "
        "      W(var w, _) => w, "
        "      X => other); "
        "    Console.WriteLine($\"got={got.Ping()}\"); "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kEscapeNeedle));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("got=7") != std::string::npos);
}

// (6) An owning match whose arm result is a VALUE derived from the binding —
// §35.5: "Copyable value payloads bind as copies", and an `int` result carries
// no borrow at all. Nothing escapes the boundary, so nothing is rejected.
BEZEL_TEST(owning_match_arm_borrow_escape_4635,
           value_result_from_binding_still_accepted_35_5) {
    auto r = runAot(prog(
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    var n := (case move o of "
        "      W(var w, _) => w.Ping(), "
        "      X => 0); "
        "    Console.WriteLine($\"n={n}\"); "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kEscapeNeedle));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("n=7") != std::string::npos);
}

// (7) An owning match whose arms bind NOTHING — §35.5's discarded-slot
// remainder, disposed at construct completion (#4625's own accepted shape).
// No binding exists to escape, so the rule must stay silent.
BEZEL_TEST(owning_match_arm_borrow_escape_4635,
           discard_only_arms_still_accepted_35_5) {
    auto r = runAot(prog(
        "    var n := (case Outer.W(new Widget(\"a\"), new Widget(\"b\")) of "
        "      W(_, _) => 1, "
        "      X => 2); "
        "    Console.WriteLine($\"n={n}\"); "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kEscapeNeedle));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("n=1") != std::string::npos);
}

// (8) STATEMENT position is untouched. §28.3 applies K8.3 "in statement
// position exactly as in expression position", but a statement-position arm
// has no construct VALUE to escape through: the adoption `using var live :=
// move w;` is the §35.5-blessed spelling and keeps working (#4617's row).
BEZEL_TEST(owning_match_arm_borrow_escape_4635,
           statement_position_adoption_still_accepted_35_5) {
    auto r = runAot(prog(
        "    var o := Outer.W(new Widget(\"a\"), new Widget(\"b\")); "
        "    case move o of "
        "      W(var w, _) => do "
        "        using var live := move w; "
        "        Console.WriteLine($\"live={live.Ping()}\"); "
        "      end, "
        "      X => () "
        "    end case; "
        "    Console.WriteLine(\"after\"); "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kEscapeNeedle));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("live=7") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("after") != std::string::npos);
}

}  // namespace
