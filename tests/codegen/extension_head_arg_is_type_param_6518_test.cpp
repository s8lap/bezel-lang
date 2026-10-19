// Issue #6518 — "Unknown type name as a generic argument in an extension head
// is silently accepted". Investigated and REFUTED: the behaviour this row pins
// is the specified behaviour, and the reported "silent acceptance" is not one.
//
// What #6518 observed (reproduced here exactly): `extension Array<NoSuchType>`
// emits no diagnostic, while `extension NoSuchTypeX<T>` does
// (`unresolved type name \`NoSuchTypeX\``). The reading behind the issue is
// that the name in an argument position is a TYPE NAME that failed to resolve.
// It is not.
//
// §26.4 (spec v0.338, *Type-parameter binding*) is explicit and rules the other
// way:
//
//   "In the extended type's argument positions, each **bare single-segment
//   non-nullable identifier** introduces a fresh block-level type parameter —
//   *always*, shadowing any same-named type in scope (the compiler warns on
//   such shadowing). This is the only implicit type-parameter binding in the
//   language, so the rule is stated exactly […] The alternative — resolving
//   the identifier against scope and only binding when unresolved — is
//   declined: it would let an unrelated type declaration elsewhere silently
//   re-mean an extension block (action at a distance)."
//
// and it extends to any nesting depth ("`extension IEnumerable<Array<T>>`
// binds `T`"). The "resolution-wins" alternative is recorded as declined in
// the changelog too (Bezel_Changelog.md v0.160, #1346: "bare single-segment
// identifier in argument position of the extended type binds fresh, always,
// with a shadowing warning — the only implicit type-parameter binding in the
// language; 'resolution-wins' declined as action-at-a-distance").
//
// So a bare identifier in an argument position is NEVER resolved as a type and
// therefore can never be unresolved. The head position is a different rule
// (§26.2's extended type, resolved by ordinary name lookup), which is exactly
// why the issue's two controls disagree — and that disagreement is the spec's,
// not a gap. What the extension actually IS is `extension Array<T>` with `T`
// spelled `NoSuchType`: `spec_6518_1` measures that directly, by reading the
// bound parameter's VALUE out of the receiver's element type (a constant
// sentinel could not see a fix that mis-binds the argument).
//
// The row also pins the affordance the spec DOES give an author who meant a
// type: a shadowing warning, which fires when the argument position's name also
// names a declared type in scope. That is the whole of §26.4's answer to the
// typo case, and it is in the lit track beside this file
// (`tests/lit/sema/extension_head_arg_shadows_declared_type_6518__warns.bzl`)
// because `runAot` fills diagnostics only on a reject, so asserting a Warning
// through it would be vacuous (#3138).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §26.4 spec_6518_1 (the issue's repro, `NoSuchType` -> `Elem`) — a bare
// single-segment identifier in the head's argument position that names NO
// declared type is a block-level type parameter, so the head is a fully
// general `Array<T>` and the parameter binds to the receiver's element type.
// Measured: `Elem` is bound to `byte` and `this[0]` yields 7.
//
// This is the row that REFUTES the issue. A compiler that resolved the
// identifier and reported it unresolved would fail it; so would one that bound
// the argument to a broken/zero type — the sentinel 7 is read out of the
// receiver, so only a correct receiver-driven inference can produce it.
BEZEL_TEST(extension_head_arg_is_type_param_6518, undeclared_name_in_arg_position_binds_as_param) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "extension Array<Elem> "
        "  public method At0(): Elem => this[0]; "
        "end; "
        "class App public static method Main(): int "
        "  var xs: Array<byte> := [7, 8]; "
        "  return int(xs.At0()); "
        "end Main; end App;"), 7);
}

// §26.4 spec_6518_2 — the same head, applied to a receiver of a DIFFERENT
// element type, binds the same (undeclared) name to that type. Together with
// spec_6518_1 this shows the head is `Array<T>` for all `T` and not a
// particular nonexistent instantiation: the block's applicability is decided by
// the block-level parameter, never by a scope lookup that #6518 expected to
// fail.
BEZEL_TEST(extension_head_arg_is_type_param_6518, undeclared_name_binds_per_receiver) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "extension Array<Elem> "
        "  public method At0(): Elem => this[0]; "
        "end; "
        "class App public static method Main(): int "
        "  var xs: Array<string> := [\"abc\"]; "
        "  return xs.At0().Length; "
        "end Main; end App;"), 3);
}

// §26.4 spec_6518_3 — the CONTRAST that #6518's own control relies on, pinned
// so the pair cannot drift: the head position is NOT an argument position, so
// an unknown name there IS resolved by ordinary name lookup and IS rejected,
// with the specific diagnostic. Asserts the message, not merely non-acceptance
// — and the program parses (the `unresolved type name` fires on a fully
// parsed extension block, not on a parse error).
BEZEL_TEST(extension_head_arg_is_type_param_6518, head_position_still_rejects_unknown_name) {
    bezel::test::JitResult r = runAot(
        "module M; "
        "extension NoSuchTypeX<T> "
        "  public method M2(): int => 0; "
        "end; "
        "class App public static method Main(): int return 0; end Main; end App;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // Assert the diagnostic's SUBSTANCE, not merely that the compile failed:
    // `r.ran == false` is equally satisfied by a parse error, and this program
    // is a fully parsed extension block (spec_6518_1 above compiles the
    // one-token-different head). A parse failure would not produce this text.
    bool sawUnresolvedHead = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("unresolved type name `NoSuchTypeX`") !=
            std::string::npos) {
            sawUnresolvedHead = true;
        }
    }
    BEZEL_EXPECT_TRUE(sawUnresolvedHead);
}