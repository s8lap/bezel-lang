// Regression test for the QUALIFIED spelling of a nested type declared inside a
// §21.10 local record — `Inner.Box` (sema, #6425, v0.338).
//
// THE GOVERNING RULE. §2.7 rule 1 (Name resolution order) is leftmost-first
// and type-wins: "Resolve the leftmost segment `A` as a bare name: local scope,
// then the enclosing type's accessible members … then the current module's
// declarations, then unambiguous imported bare names … then import aliases. If
// `A` resolves to a *type*, the rest of the chain resolves against that type
// (nested type, enum constant, static member — each segment in turn)."
// (§2.7, v0.338.)
//
// A §21.10 local type is one of "the current module's declarations" in that
// list: §21.10 puts its name in scope "from its declaration statement to the
// end of the innermost enclosing statement block", and "within its scope the
// name is an ordinary type reference in body positions" (§21.10, v0.338).
//
// The segments after the head are §11.3's: §11.3 is what makes a nested type a
// MEMBER of the type that declares it, and it says in so many words that "a
// local record's own body may itself contain §11.6 nested member types, which
// follow their own rules" — and §11.6's rule for such a type is that it "is
// referenced from outside as `Outer.Nested`" (§11.6, v0.338).
//
// So `Inner.Box` is an ordinary rule-1 chain with a local-type head.
//
// THE DEFECT (#6425), and why it was a LOOKUP-PATH gap rather than a missing
// registration. The registration was already there: fix #6411 added the §11.3
// loop at `resolveNamed`'s §21.10 arm, which is what makes the BARE spelling
// work and is pinned by `local_type_nested_type_6411_test.cpp`. What was
// missing is the multi-part path's ability to find the HEAD. A local type is
// registered in `ctx_.module()->children` and in no `Scope` at all, so the only
// rule-1 source that can see one is the module-children fallback in
// `resolveType`'s `!sym` block — and that fallback is gated on
// `t.name->parts.size() == 1`. A multi-segment spelling had no path to a local
// type: `lookupName(first, scope)` walked the Scope chain and found nothing.
//
// MEASURED on this branch's base (bezelc rebuilt from the base commit): both
//
//   var b: Inner.Box := new Inner.Box(7); return b.W;      (enclosing body)
//   public method Get(): Inner.Box return new Inner.Box(7);  (Inner's own body)
//
// rejected with "unresolved type name `Inner`" — a diagnostic that blames the
// chain HEAD, which §2.7 rule 1 had already resolved as a bare name elsewhere
// in the very same body (`new Inner()` compiles), and never names the segment
// that actually failed.
//
// ORACLE. runAot (the real AOT path) on every row. Each accept row asserts
// `r.ran` as well as the absence of a compile-time failure, so no
// skip-absorbing guard can stand in for the program.
//
// ROWS. Rows 1, 2 and 6 are red on the base. Rows 3, 4 and 5 are the controls
// that say the newly-accepted set is exactly the one rule-1 spelling and not
// something wider: row 3 is the §11.6 path that already worked, row 4 is
// §21.10's two-methods rule (a sibling method's local type must stay out of
// reach), row 5 is §21.10's member-signature prohibition.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// The MODULE-level `record Box(V: int)` is on a DIFFERENT shape from every
// local `record Box(W: int)` below, so a wrong binding is observable rather
// than silent: reading `.W` can only succeed against the nested type.
const char* kPreamble =
    "module M; import System; public record Box(V: int); ";
const char* kTrailer =
    " public class Program public static method Main() "
    "Console.WriteLine(\"x\"); end Main; end Program; ";

}  // namespace

// ── 1. THE QUALIFIED HEAD RESOLVES, FROM THE ENCLOSING BODY ──────────────
//
// The issue's own repro. `new Inner()` on a sibling line compiles clean —
// §2.7 rule 1 resolves that bare name through `lookupBareTypeName` — while
// `Inner.Box` on the same shape was rejected as `unresolved type name `Inner``.
// Reading `.W` (a field of `Inner.Box` alone) is the discriminator: a
// mis-binding to the module-level `Box` would report a missing member.
BEZEL_TEST(local_type_nested_qualified_6425,
           spec_2_7_qualified_head_of_a_local_type_resolves_its_nested_type) {
    const std::string src =
        std::string(kPreamble) +
        "public class Outer public method M(): int "
        "record Inner "
        "  public record Box(W: int); "
        "end Inner; "
        "var i := new Inner(); var b: Inner.Box := new Inner.Box(7); "
        "_ := i; return b.W; "
        "end M; end Outer; " +
        kTrailer;
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// ── 2. AND FROM THE LOCAL TYPE'S OWN MEMBER BODIES ───────────────────────
//
// The same chain, one scope in. §21.10 puts the local type's own name in
// scope inside its own body ("the name IS in scope inside its own body"), so
// the head is in scope here by the same rule as in row 1 — and a fix that only
// reached the enclosing block would leave this row rejected.
BEZEL_TEST(local_type_nested_qualified_6425,
           spec_11_3_qualified_nested_type_resolves_inside_the_local_types_own_body) {
    const std::string src =
        std::string(kPreamble) +
        "public class Outer public method M(): int "
        "record Inner "
        "  public record Box(W: int); "
        "  public method Get(): Inner.Box return new Inner.Box(7); end Get; "
        "end Inner; "
        "var i := new Inner(); return i.Get().W; "
        "end M; end Outer; " +
        kTrailer;
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// ── 3. CONTROL: THE §11.6 PATH ALREADY WORKED AND MUST KEEP WORKING ──────
//
// `Outer.Inner.Box` goes through `resolveTypeDecl`, which has always carried
// the §11.3 registration and whose nested TypeSymbols live in the enclosing
// type's member list. It compiled clean on the base and must compile clean
// now — a fix that disturbed the ordinary path would break this row, and one
// that did nothing at all would leave it identical, which is the point: rows 1
// and 2 are about the LOCAL-type window and not about the rule.
BEZEL_TEST(local_type_nested_qualified_6425,
           spec_11_6_qualified_nested_type_of_a_nested_type_still_resolves) {
    const std::string src =
        std::string(kPreamble) +
        "public class Outer public record Inner "
        "  public record Box(W: int); "
        "end Inner; end Outer; "
        "public class Program public static method Main() "
        "var b: Outer.Inner.Box := new Outer.Inner.Box(7); "
        "Console.WriteLine(b.W); end Main; end Program; ";
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(r.ran);
}

// ── 4. CONTROL: A SIBLING METHOD'S LOCAL TYPE STAYS OUT OF REACH ────────
//
// §21.10 "Scope and name discipline": "Two *different* methods may each
// declare `record Row(…)`: the names never meet, and the types are distinct."
// The head a qualified chain may resolve is therefore a LOCAL type whose
// recorded block extent CONTAINS this reference — the same
// `localScopeEndOffset` test the single-part fallback applies. This row is the
// over-admission control: a head lookup that ignored the extent would let
// `B` reach `A`'s `Inner`, and the program would compile clean meaning a type
// §21.10 puts out of scope there.
BEZEL_TEST(local_type_nested_qualified_6425,
           spec_21_10_a_sibling_methods_local_type_is_not_reached_by_a_qualified_head) {
    const std::string src =
        std::string(kPreamble) +
        "public class Outer "
        "  public method A(): int "
        "    record Inner "
        "      public record Box(W: int); "
        "    end Inner; "
        "    var b: Inner.Box := new Inner.Box(7); return b.W; "
        "  end A; "
        "  public method B(): int "
        "    var b: Inner.Box := new Inner.Box(1); return b.W; "
        "  end B; "
        "end Outer; " +
        kTrailer;
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        hasErrorContaining(r, "unresolved type name `Inner`"));
}

// ── 5. CONTROL: A MEMBER SIGNATURE IS STILL A PROHIBITED POSITION ───────
//
// §21.10: "The name may **not** appear in any **member signature of an
// enclosing or sibling declaration** — parameter, return, field/property/
// indexer, generic-constraint, or base/interface positions of the enclosing
// method itself, or of any member of any type declared outside the local type
// — which is a compile error naming that rule." A qualified `Inner.Box` in a
// return position is rejected today and stays rejected: the fix reaches
// qualified HEADS in bodies only, and admitting the member-signature spelling
// is a separate change with its own diagnostic.
BEZEL_TEST(local_type_nested_qualified_6425,
           spec_21_10_qualified_local_type_head_in_a_member_signature_is_still_rejected) {
    const std::string src =
        std::string(kPreamble) +
        "public class Outer "
        "  public method A(): Inner.Box "
        "    record Inner "
        "      public record Box(W: int); "
        "    end Inner; "
        "    return new Inner.Box(7); "
        "  end A; "
        "end Outer; " +
        kTrailer;
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        hasErrorContaining(r, "unresolved type name `Inner`"));
}

// ── 6. THE HEAD IS NOW RESOLVED, SO THE DIAGNOSTIC NAMES THE SEGMENT ────
//
// Once `Inner` resolves as a type, §2.7 rule 1 hands the REST of the chain to
// §11.3's descent, which names the segment that is missing. This is a
// DIAGNOSTIC-TEXT flip and it is the honest one: the base blamed the head,
// which was not what failed. Both outcomes REJECT; only the message moves.
BEZEL_TEST(local_type_nested_qualified_6425,
           spec_11_3_qualified_head_names_the_segment_that_is_missing) {
    const std::string src =
        "module M; import System; "
        "public class Outer public method M(): int "
        "record Inner "
        "  public record Box(W: int); "
        "end Inner; "
        "var b: Inner.Missing := new Inner.Missing(1); _ := b; return 0; "
        "end M; end Outer; " +
        std::string(kTrailer);
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "type `Inner` has no nested type named `Missing` (§11.3)"));
}