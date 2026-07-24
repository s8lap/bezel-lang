// Spec-derived ACCEPT controls for issue #2181 — widening
// MethodOwnership::collectIdentifiersInStmt to walk the control-flow-nested
// statements of a block-bodied lambda (if / while / for / do-end / try / case)
// so a capture appearing only there is visible to every closure-capture escape
// leg (§9.7 record/span, §28.9 owned-local, §31.1 catch, §27.5 ref/out).
//
// The REJECT side — a borrow-bearing record / bare span / owned class local
// captured ONLY under an `if`/`while`/`for` of a RETURNED block lambda — is
// pinned by the lit tests (tests/lit/ownership/closure_capture_escape_2181__*).
// This file pins the ACCEPT side: the widening only ADDS the control-flow-
// nested identifiers to the SAME escape check, whose ESCAPE gate is unchanged,
// so a NON-escaping (locally invoked) capture and a PARAM-sourced capture must
// stay accepted and ASan-clean — the over-reject failure mode. Each runs under
// the AOT + ASan/LSan oracle (runAotChecked).
//
// per §9.7 (v0.220), §28.9, §28.3

#include <string>

#include "support/aot_support.hpp"       // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

// ===========================================================================

// §9.7: a borrow-bearing record captured ONLY under an `if` inside a block
// lambda that is INVOKED LOCALLY (bound to `f`, called before scope end while
// `local`/`h` are still alive). The closure never escapes the source's scope,
// so the §9.7 closure bullet does not apply — the widened collector must not
// turn this into a rejection. == 7.
BEZEL_TEST(closure_capture_escape_2181, record_capture_in_if_block_lambda_local_invoke_accepted) {
    std::string src =
        "module M; record Holder(S: ArraySpan<int>); class C "
        "  public static method Main(): int "
        "    var local := new Array<int>(); local.Add(7); "
        "    var h := new Holder(local.AsSpan()); "
        "    var f := lambda(): int => do "
        "      if h.S.Length > 0 then return h.S[0]; end; "
        "      return 0; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// §9.7: an ESCAPING (returned) block lambda whose control-flow-nested capture's
// carried source is a PARAMETER — the record views the caller's array, which
// outlives the callee frame (a parameter is not an escape-bound obligation,
// mirroring the #2144 param-sourced accept). The widened collector makes `h`
// visible under the `if`, but carriedEscapeBoundSourceLiveAt resolves no
// escape-bound source, so it stays accepted. == 7.
BEZEL_TEST(closure_capture_escape_2181, param_source_capture_in_if_block_lambda_returned_accepted) {
    std::string src =
        "module M; record Holder(S: ArraySpan<int>); class C "
        "  public static method Make(a: Array<int>): () -> int "
        "    var h := new Holder(a.AsSpan()); "
        "    return lambda(): int => do "
        "      if h.S.Length > 0 then return h.S[0]; end; "
        "      return 0; "
        "    end; "
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

// §28.9: an owned class local captured ONLY under an `if` inside a block lambda
// that is INVOKED LOCALLY — the non-escaping sibling of the owned-local reject.
// Capturing an owned local as a borrow is legal while the closure does not
// escape the owner's scope. The `if` condition uses a lambda-local so only `r`
// is a method-scope capture. == 5.
BEZEL_TEST(closure_capture_escape_2181, owned_local_capture_in_if_block_lambda_local_invoke_accepted) {
    std::string src =
        "module M; class Res public method V(): int return 5; end V; end Res; "
        "class C "
        "  public static method Main(): int "
        "    var r := new Res(); "
        "    var f := lambda(): int => do "
        "      var n := 1; "
        "      if n > 0 then return r.V(); end; "
        "      return 0; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// §9.7: a VALUE-ONLY record (no borrow-carrying field — not borrow-bearing)
// captured under an `if` of a RETURNED block lambda. With no carried borrow
// there is no lifetime constraint, so the escape is legal — the widening must
// add identifiers to the check WITHOUT widening the TYPE gate to "any record".
// == 7.
BEZEL_TEST(closure_capture_escape_2181, value_record_capture_in_if_block_lambda_returned_accepted) {
    std::string src =
        "module M; record Pair(X: int, Y: int); class C "
        "  public static method Make(): () -> int "
        "    var p := new Pair(3, 4); "
        "    return lambda(): int => do "
        "      if p.X > 0 then return p.X + p.Y; end; "
        "      return 0; "
        "    end; "
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
