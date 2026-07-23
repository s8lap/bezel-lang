// Spec-derived tests for issue #2144 — a borrow-bearing record captured by an
// ESCAPING closure is rejected (§9.7: "A borrow-bearing record may not be
// captured by a closure that escapes its source's scope").
//
// Before the fix, the closure-capture escape checks consulted only the
// captured local's OWNED state (§28.9's direct owned-local capture) and never
// the borrow-bearing-record classification, so a record local — a value, never
// Owned — slipped through every escape route: `return lambda …` capturing it
// compiled and SEGFAULTED under the AOT+ASan oracle once the record's carried
// borrow's source auto-dropped (§28.3). The REJECT side (return, holder-local
// return, outer-local store, field store, span-local and generic-record
// captures) is pinned by the lit tests
// (tests/lit/ownership/record_closure_capture_escape_2144__*). This file pins
// the ACCEPT side — §9.7's rule constrains only a closure that ESCAPES the
// source's scope, so the nearby legal shapes must stay accepted and ASan-clean
// (over-rejection is the classic failure mode for this family). Each test
// returns a distinct value so a silently wrong compile is caught too.
//
// // per §9.7, §28.3, §28.9, §4.14, §4.15 (v0.220, #2144)

#include <string>

#include "support/aot_support.hpp"       // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

// ===========================================================================
// ACCEPT — non-escaping and outliving-source captures stay legal.
// ===========================================================================

// §9.7: a NON-ESCAPING capture — the record-capturing closure is bound to a
// same-scope local and invoked before scope end; `local` outlives every use.
// The closure never escapes the source's scope, so the §9.7 closure bullet
// does not apply. == 7.
BEZEL_TEST(record_closure_capture_escape_2144, local_invoke_accepted) {
    std::string src =
        "module M; record Holder(S: ArraySpan<int>); class C "
        "  public static method Main(): int "
        "    var local := new Array<int>(); local.Add(7); "
        "    var h := new Holder(local.AsSpan()); "
        "    var f := lambda(): int => h.S[0]; "
        "    return f(); "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// §9.7: an escaping capture whose carried source is a PARAMETER — the record
// views the caller's array, which outlives the callee frame (a parameter is
// not an escape-bound obligation; mirrors the #1966 param-sourced-span
// accept). The caller keeps `arr` alive across the invocation, so the read is
// sound. == 7.
BEZEL_TEST(record_closure_capture_escape_2144, param_source_capture_returned_accepted) {
    std::string src =
        "module M; record Holder(S: ArraySpan<int>); class C "
        "  public static method Make(a: Array<int>): () -> int "
        "    var h := new Holder(a.AsSpan()); "
        "    return lambda(): int => h.S[0]; "
        "  end Make; "
        "  public static method Main(): int "
        "    var arr := new Array<int>(); arr.Add(7); "
        "    var f := C.Make(arr); "
        "    return f(); "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// §9.7: the CLASS-field record sibling of local_invoke_accepted (issue #2144
// is "general: class- and span-field alike" — so is the accept side). == 3.
BEZEL_TEST(record_closure_capture_escape_2144, class_field_record_local_invoke_accepted) {
    std::string src =
        "module M; class Res public method Val(): int return 5; end Val; end Res; "
        "record RefRec(W: Res?, N: int); class C "
        "  public static method Main(): int "
        "    var local := new Res(); "
        "    var h := new RefRec(local, 3); "
        "    var f := lambda(): int => h.N; "
        "    return f(); "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// §9.7: a bare SPAN local captured and invoked locally — the span sibling of
// local_invoke_accepted (§4.14/§4.15: a span is a borrow-bearing record
// without a user TypeDecl; non-escaping capture of it is equally legal). == 9.
BEZEL_TEST(record_closure_capture_escape_2144, span_local_invoke_accepted) {
    std::string src =
        "module M; class C "
        "  public static method Main(): int "
        "    var local := new Array<int>(); local.Add(9); "
        "    var s := local.AsSpan(); "
        "    var f := lambda(): int => s[0]; "
        "    return f(); "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 9);
}

// §9.7: a VALUE-ONLY record (no borrow-carrying field — not borrow-bearing)
// captured by a closure that ESCAPES. Records are copied into the capture;
// with no carried borrow there is no lifetime constraint, so the escape is
// legal (the type gate must not widen to "any record"). == 7.
BEZEL_TEST(record_closure_capture_escape_2144, value_record_capture_returned_accepted) {
    std::string src =
        "module M; record Pair(X: int, Y: int); class C "
        "  public static method Make(): () -> int "
        "    var p := new Pair(3, 4); "
        "    return lambda(): int => p.X + p.Y; "
        "  end Make; "
        "  public static method Main(): int "
        "    var f := C.Make(); "
        "    return f(); "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
