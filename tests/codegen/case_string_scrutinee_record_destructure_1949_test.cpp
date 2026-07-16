// fix #1949 / §19.3/§19.5 (v0.182): a record-destructuring pattern
// `case s of Pt(a, b)` over a scrutinee whose STATIC type is `string` faulted
// at run time (SIGSEGV).
//
// §19.5: a bare type pattern "matches only non-null values" of the named type;
// a `string` is never a record, so `case (s: string) of Pt(a, b)` must cleanly
// NOT match — take the `_` arm — never crash. §19.3 lowers a record type-test
// to a runtime is-a walk (`emitIsA`), which loads the scrutinee's word 0 and
// dereferences it as a type descriptor. A `string` is a vtable-less
// `bz_string*` whose word 0 is its `char* data`, not a descriptor — walking it
// faults.
//
// This is the record-path analogue of the landed #1938 (which guarded the same
// fault for a string boxed in an `Object`, and for the class/interface
// NamedPattern + `var x: C` forms). #1938 routed the record is-a through
// `emitStringGuardedIsA`, but that helper's `mayBeString` predicate is FALSE
// for a scrutinee whose STATIC type is `string` (string is neither `Object` nor
// an interface), so a statically-string-typed scrutinee fell through to a plain
// `emitIsA` and faulted. The class/interface NamedPattern path never had this
// bug because it gates the is-a on an `objectLike` static check first — a
// `string`-typed scrutinee is rejected before reaching the is-a. The record
// path (src/codegen/LlvmBackend_Exprs2.cpp) lacked that gate; the fix adds the
// same `objectLike` gate to the record-destructuring pointer branch.
//
// Fail-without/pass-with: WITHOUT the fix `case (s: string) of Pt(a, b)`
// SIGSEGVs at run time; the AOT harness reports a signalled process as
// ran==false → the `run()` helper returns -999, which != the spec-stated
// no-match value. WITH the fix the program runs to normal termination and
// returns the `_`-arm value. Observable is the process exit code.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// -- The bug: a statically string-typed scrutinee must no-match, not fault -----

// §19.3/§19.5 (#1949): `case (s: string) of Pt(a, b)` where the scrutinee's
// static type is `string` must take the `_` arm (a string is never a Pt) — 9.
// WITHOUT the objectLike gate the record is-a runs a plain emitIsA over the
// vtable-less bz_string* → SIGSEGV → -999.
BEZEL_TEST(case_string_scrutinee_record_destructure_1949, string_scrutinee_no_fault) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(x: int, y: int); "
        "class Program public static method Main(): int "
        "  var s: string := \"hi\"; "
        "  return case s of Pt(a, b) => a + b, _ => 9 end; "
        "end Main; end Program;"), 9);
}

// §19.3/§19.5 (#1949): a string PARAMETER (also statically `string`) is the
// same fault surface — the record destructuring must cleanly no-match.
BEZEL_TEST(case_string_scrutinee_record_destructure_1949, string_param_no_fault) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(x: int, y: int); "
        "class P "
        "  public static method Case(s: string): int "
        "    return case s of Pt(a, b) => a + b, _ => 9 end; "
        "  end Case; "
        "end P; "
        "class Program public static method Main(): int "
        "  return P.Case(\"world\"); "
        "end Main; end Program;"), 9);
}

// -- The gate must not break genuine record destructuring (no false negatives) --

// §19 / §9.4 (#1949): a genuine record value scrutinee still destructures and
// binds its positional fields (3 + 4 = 7) — the objectLike gate is on the
// pointer/reference branch, which a record VALUE scrutinee never takes.
BEZEL_TEST(case_string_scrutinee_record_destructure_1949, record_value_still_destructures) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(x: int, y: int); "
        "class Program public static method Main(): int "
        "  var p := new Pt(3, 4); "
        "  return case p of Pt(a, b) => a + b, _ => 9 end; "
        "end Main; end Program;"), 7);
}

// §19.3 / §9.4 (#1949): a boxed record in an `Object` still destructures (the
// object-like reference branch, guarded for a string-in-Object per #1938, still
// matches a real Pt) — 5 + 6 = 11. Guards against the objectLike gate being too
// aggressive and rejecting a legitimate boxed-record match.
BEZEL_TEST(case_string_scrutinee_record_destructure_1949, boxed_record_still_destructures) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(x: int, y: int); "
        "class Program public static method Main(): int "
        "  var o: Object := new Pt(5, 6); "
        "  return case o of Pt(a, b) => a + b, _ => 9 end; "
        "end Main; end Program;"), 11);
}
