// Regression for #2496: an unknown case/member name on a choice type used as
// a constructor (`Result<int, string>.Err("bad")`, `Either<int,string>.Bogus(5)`)
// was silently accepted by sema and degraded to a zero-initialized value in
// codegen (the #2434 fallback fired an `internal:` remark, exit 0) instead of
// being rejected. The stdlib `Result<T, E>` (stdlib §2.20) has cases `Ok`/`
// `Error` — there is no `Err` case — so the issue's own `Result<T,E>.Err(...)`
// repro is a misspelled case name that a correct compiler must diagnose, not
// silently miscompile.
//
// Root cause: ExprTypecheck_Calls.cpp's "genuinely-unknown member" reject
// (issue #414) gated on the receiver being a known-complete USER type, but its
// kind check listed Class/Record/Enum and omitted Choice — so a choice-typed
// receiver fell through with no diagnostic and no resolved callee, and codegen
// lowered null → zero. The fix adds Choice to that gate (and to the
// synthesized-Serialize/Deserialize exemption so a valid serialized choice
// is not falsely rejected). Spec §35.2 (Members) / §35.3 (Construction) —
// construction is qualified and resolves as enum constants do; an unknown name
// is not a member of the choice (§35.2). v0.236.
//
// Per tests/README.md: runAot's WIFSIGNALED path also synthesizes an Error
// diagnostic for a runtime crash, so a genuine compile-time rejection must be
// distinguished by asserting on the diagnostic TEXT (the "no member" wording),
// never by exit code or `!r.ran` alone.

#include "support/aot_support.hpp"   // runAot — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// True iff the program's compile produced an Error-severity diagnostic whose
// message contains `keyword`. Without the fix, the wrong-case-name programs
// compile cleanly (only a non-Error `internal:` remark) and this returns false.
bool hasError(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// The issue's literal repro: `Result<int, string>.Err("bad")`. `Err` is not a
// case of `Result<T, E>` (the cases are `Ok`/`Error`, stdlib §2.20). Without
// the fix this silently degraded to zero (exit 0, only an `internal:` remark);
// with the fix it is a clean compile error naming the missing member.
BEZEL_TEST(choice_unknown_member_reject_2496,
           stdlib_result_wrong_case_name_err_rejected) {
    const std::string program =
        "module M; class C public static method Main(): int "
        "  var r: Result<int, string> := Result<int, string>.Err(\"bad\"); "
        "  return 0; end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(program, "no member `Err`"));
    BEZEL_EXPECT_TRUE(hasError(program, "Result"));
}

// The same defect is NOT specific to `Result`: a user-defined generic choice
// with an identically-misspelled case name also silently degraded before the
// fix (the issue's "Either.Right works, Result.Err does not" framing was an
// artifact of comparing a real case to a misspelled one). The fix rejects
// unknown member names uniformly across all choice kinds.
BEZEL_TEST(choice_unknown_member_reject_2496,
           user_choice_wrong_case_name_rejected) {
    const std::string program =
        "module M; "
        "choice Either<L, R> Left(l: L), Right(r: R); end Either; "
        "class C public static method Main(): int "
        "  var e: Either<int, string> := Either<int, string>.BogusName(5); "
        "  return 0; end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(program, "no member `BogusName`"));
}

// The CORRECT error-case name (`Error`, stdlib §2.20) must still construct —
// and with the right runtime value: the right case tag AND the right payload.
// Spec §35.3: construction binds payload arguments to the case's payload
// fields. Returns 0 only if `Error("bad")` lands in the `Error(var m)` arm
// with m == "bad"; any tag/payload corruption returns nonzero.
BEZEL_TEST(choice_unknown_member_reject_2496,
           stdlib_result_correct_error_case_value_correct) {
    const std::string program =
        "module M; class C public static method Main(): int "
        "  var err: Result<int, string> := Result<int, string>.Error(\"bad\"); "
        "  case err of "
        "    Ok(var v)    => return 1;, "
        "    Error(var m) => return (if m = \"bad\" then 0 else 2); "
        "  end; "
        "  return 3; end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 0);
}

// And the CORRECT success-case name (`Ok`) with a value payload, including the
// second-type-parameter instantiation shape the issue also exercised
// (`Result<string, int>.Error(9)`) — verifies the fix does not disturb the
// working case-construction path for either case or type-parameter position.
BEZEL_TEST(choice_unknown_member_reject_2496,
           stdlib_result_ok_and_second_typeparam_value_correct) {
    const std::string program =
        "module M; class C public static method Main(): int "
        "  var ok: Result<int, string> := Result<int, string>.Ok(42); "
        "  case ok of "
        "    Ok(var v)    => do if v <> 42 then return 1; end; end, "
        "    Error(var m) => return 2; "
        "  end; "
        "  if not ok.IsOk() then return 3; end; "
        "  var err2: Result<string, int> := Result<string, int>.Error(9); "
        "  case err2 of "
        "    Ok(var s)    => return 4;, "
        "    Error(var e) => do if e <> 9 then return 5; end; end "
        "  end; "
        "  return 0; end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 0);
}
