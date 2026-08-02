// Regression tests for issue #2423 — an unresolved member access on a
// declaration-less built-in exception type compiled with ZERO diagnostic and,
// at runtime, read uninitialized stack memory through the destination local
// (confirmed to leak raw pointer-shaped bytes; no crash, no ASan report).
//
// Root cause: `ExprTC::resolveMemberAccess` (src/sema/ExprTypecheck_Calls.cpp)
// special-cases member access on a declaration-less built-in exception type
// (`declaration == nullptr`, e.g. a caught `Exception`/`ArgumentException`/…)
// by matching the member NAME against hand-written lists (Message,
// StackTrace, CauseChain, ParamName). When the name matched none of them, the
// function fell all the way through to a bare `return;` with NO diagnostic —
// `ctx_.setExprType` was simply never called. Codegen mirrors the gap
// (`Emitter::lowerExpr`'s MemberAccessExpr handling, LlvmBackend_Exprs.cpp,
// returns nullptr for the same unmatched case), and `Emitter::lowerLocalDecl`
// (LlvmBackend_Setup.cpp) has no fallback when the initializer fails to
// lower: the destination `alloca` is declared and later loaded, but never
// stored to — a textbook uninitialized-stack read.
//
// The fix adds the missing diagnostic at the tail of `resolveMemberAccess`'s
// `!targetType` branch: any member access on a declaration-less built-in
// exception type (name ends "Exception") or the built-in `ExceptionInfo`
// record whose member name matched none of the special-cased accessors above,
// and is not one of the universal Object members (ToString/Equals/
// GetHashCode/GetType/ReferenceEquals/Dispose — resolved independently by the
// enclosing CallExpr machinery), is now a compile error.
//
// language spec §31.4 "Standard exception types" (v0.235) declares the
// closed `Exception`/`ArgumentException` surface; stdlib §2.17 "Built-in
// exception types (cross-reference)" (v0.235) names the three exception
// types whose documented surface goes beyond `Exception`'s:
// `SerializationException.{Offset,Line,Column}` (stdlib §2.15) and
// `DomainFaultedException.FaultStackTrace` (stdlib §6.8) are genuinely
// DECLARED but have NO implementation at any layer (confirmed: no sema case,
// no codegen lowering, no runtime field) — tracked separately by #2337/#2408,
// which this fix does not implement. Rejecting them with the ordinary
// "no member" wording would misstate a documented fact, so they get a
// distinct, accurate diagnostic instead ("declared ... but not yet
// implemented"), which still closes the same uninitialized-stack-read hole
// for them without implementing the getters.
// `TypeInitializationException.InnerException` is the fourth type §2.17
// names — but unlike the other three it IS fully implemented in codegen
// (LlvmBackend_Exprs.cpp reads byte offset 16 by matching
// `ma->member == "InnerException"` structurally, independent of sema ever
// setting `ma`'s exprType) and is exercised by
// tests/codegen/type_init_inner_exception_1107_test.cpp and others; it is
// therefore exempted from the new reject so this fix does not regress a
// working, tested feature.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Returns true iff the run produced an Error diagnostic containing `keyword`.
// Per tests/README.md: runAot's WIFSIGNALED path also synthesizes an Error
// diagnostic for a runtime crash, so a genuine compile-time rejection must be
// distinguished by asserting on the diagnostic TEXT (this helper), never by
// exit code or `!r.ran` alone — mirrors the precedent in
// tests/codegen/user_exception_message_595_test.cpp.
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

// ── Group 1: the core #2423 repro — bogus member on a caught `Exception` ──

// #2423 repro (property-read form): `e.ThisMemberDoesNotExist` on a caught
// `Exception` must be a compile error naming the member and the type, not a
// silent accept. Before the fix: compiled at exit 0 with zero diagnostics
// (verified on origin/main) and read uninitialized stack memory at runtime.
BEZEL_TEST(codegen_exception_unresolved_member_2423,
           bogus_property_read_on_caught_exception_rejected_spec_31_4) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  try "
        "    throw new Exception(\"x\"); "
        "  catch var e: Exception => do "
        "    var x: long? := e.ThisMemberDoesNotExist; "
        "  end end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        hasError(program,
                 "no member `ThisMemberDoesNotExist` on `Exception`"));
}

// #2423 / a plausible real-world typo (`Mesage` for `Message`) must also be
// rejected — the generalized claim in the issue, not just the exact repro.
BEZEL_TEST(codegen_exception_unresolved_member_2423,
           typo_mesage_for_message_rejected) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  try "
        "    throw new Exception(\"x\"); "
        "  catch var e: Exception => do "
        "    Console.WriteLine(e.Mesage); "
        "  end end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(program, "no member `Mesage` on `Exception`"));
}

// #2423 (method-call form): `e.Bogus()` — the callee is the same
// MemberAccessExpr shape; must be rejected identically to the property form.
BEZEL_TEST(codegen_exception_unresolved_member_2423,
           bogus_method_call_form_rejected) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  try "
        "    throw new Exception(\"x\"); "
        "  catch var e: Exception => do "
        "    var s := e.Bogus(); "
        "  end end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(program, "no member `Bogus` on `Exception`"));
}

// #2423 (catch-filter `when` guard, §31.2 CatchArm grammar): a bogus member
// used as the filter expression must be rejected too, not silently typed
// (which would otherwise make the guard's `bool` requirement fail lower down
// on an untyped expression instead of naming the real problem).
BEZEL_TEST(codegen_exception_unresolved_member_2423,
           bogus_member_in_catch_filter_rejected) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  try "
        "    throw new Exception(\"x\"); "
        "  catch var e: Exception when e.Bogus => return 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(program, "no member `Bogus` on `Exception`"));
}

// #2423 (freshly-constructed, not caught): the receiver need not be a
// catch-bound variable — any expression typed as a declaration-less built-in
// exception hits the same gap.
BEZEL_TEST(codegen_exception_unresolved_member_2423,
           bogus_member_on_fresh_construction_rejected) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var e := new Exception(\"x\"); "
        "  var s := e.Bogus; "
        "  _ := e; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(program, "no member `Bogus` on `Exception`"));
}

// #2423 on a DIFFERENT built-in exception (ArgumentException) — confirms the
// fix is not special-cased to plain `Exception`.
BEZEL_TEST(codegen_exception_unresolved_member_2423,
           bogus_member_on_argumentexception_rejected) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ArgumentException(\"bad\", \"p\"); "
        "  catch var e: ArgumentException => do "
        "    var s := e.NotAThing; "
        "  end end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        hasError(program, "no member `NotAThing` on `ArgumentException`"));
}

// ── Group 2: legitimate §31.4 members keep working (no false rejects) ──────

// §31.4: Message, StackTrace, CauseChain, and ArgumentException.ParamName
// remain accepted — the fix must not regress the documented surface.
BEZEL_TEST(codegen_exception_unresolved_member_2423,
           documented_members_still_accepted_spec_31_4) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ArgumentException(\"bad\", \"p\"); "
        "  catch var e: ArgumentException => do "
        "    var m := e.Message; "
        "    var st := e.StackTrace; "
        "    var cc := e.CauseChain; "
        "    var pn := e.ParamName; "
        "    return (if m.Length > 0 and cc.Count = 0 and pn = \"p\" then 42 "
        "             else 1); "
        "  end end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// ── Group 3: SerializationException.{Offset,Line,Column} — stdlib §2.15 ────
// Genuinely declared (stdlib §2.15) but not implemented at any layer
// (#2337/#2408). Before this fix: silently accepted, reading uninitialized
// stack memory (the #2408 finding this issue generalized). After: a distinct
// "declared ... not yet implemented" compile error, NOT the generic
// "no member" wording (which would misstate a documented fact).
BEZEL_TEST(codegen_exception_unresolved_member_2423,
           serializationexception_offset_declared_not_implemented_stdlib_2_15) {
    const std::string program =
        "module M; import System.Serialization; "
        "record Pt(X: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"X\\\": \\\"bad\\\"}\"); "
        "  try "
        "    var p := Pt.Deserialize(d); "
        "  catch var e: SerializationException => do "
        "    var o: long? := e.Offset; "
        "  end end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        hasError(program,
                 "member `Offset` on `SerializationException` is declared "
                 "(stdlib \xC2\xA7""2.15) but is not yet implemented"));
    BEZEL_EXPECT_TRUE(hasError(program, "#2337"));
}

BEZEL_TEST(codegen_exception_unresolved_member_2423,
           serializationexception_line_declared_not_implemented_stdlib_2_15) {
    const std::string program =
        "module M; import System.Serialization; "
        "record Pt(X: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"X\\\": \\\"bad\\\"}\"); "
        "  try "
        "    var p := Pt.Deserialize(d); "
        "  catch var e: SerializationException => do "
        "    var l: int? := e.Line; "
        "  end end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        hasError(program,
                 "member `Line` on `SerializationException` is declared "
                 "(stdlib \xC2\xA7""2.15) but is not yet implemented"));
}

BEZEL_TEST(codegen_exception_unresolved_member_2423,
           serializationexception_column_declared_not_implemented_stdlib_2_15) {
    const std::string program =
        "module M; import System.Serialization; "
        "record Pt(X: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  using var d := new JsonDeserializer(\"{\\\"X\\\": \\\"bad\\\"}\"); "
        "  try "
        "    var p := Pt.Deserialize(d); "
        "  catch var e: SerializationException => do "
        "    var col: int? := e.Column; "
        "  end end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        hasError(program,
                 "member `Column` on `SerializationException` is declared "
                 "(stdlib \xC2\xA7""2.15) but is not yet implemented"));
}

// ── Group 4: DomainFaultedException.FaultStackTrace — stdlib §6.8 ──────────
// Same category as Group 3: declared, unimplemented, distinct diagnostic.
BEZEL_TEST(codegen_exception_unresolved_member_2423,
           domainfaultedexception_faultstacktrace_declared_not_implemented_stdlib_6_8) {
    const std::string program =
        "module M; import System.Threading; "
        "class Worker public static method Fail() "
        "  throw new Exception(\"domain-fault-test\"); "
        "end Fail; end Worker; "
        "class Program public static async method Main(): Task<int> "
        "  using var t := Domain.Spawn(Worker.Fail); "
        "  try "
        "    await t; "
        "    return -1; "
        "  catch var e: DomainFaultedException => do "
        "    var fst := e.FaultStackTrace; "
        "  end end; "
        "  return 0; "
        "end Main; end Program;";
    BEZEL_EXPECT_TRUE(
        hasError(program,
                 "member `FaultStackTrace` on `DomainFaultedException` is "
                 "declared (stdlib \xC2\xA7""6.8) but is not yet implemented"));
    BEZEL_EXPECT_TRUE(hasError(program, "#2337"));
}

// ── Group 5: TypeInitializationException.InnerException keeps working ─────
// The one member of the "beyond Exception's surface" set (stdlib §2.17) that
// IS fully implemented; must stay exempt from the new reject. Mirrors
// tests/codegen/type_init_inner_exception_1107_test.cpp's established repro
// shape as a direct guard on this fix's exemption condition.
BEZEL_TEST(codegen_exception_unresolved_member_2423,
           typeinitializationexception_innerexception_not_regressed) {
    const std::string program =
        "module M; "
        "class FS public static _v: int := Throw(); "
        "  private static method Throw(): int "
        "    throw new ArgumentException(\"inner-msg\"); return 0; "
        "  end Throw; "
        "end FS; "
        "class C public static method Main(): int "
        "  try var _ := FS._v; "
        "  catch var ex: TypeInitializationException => do "
        "    if ex.InnerException is not null then return 55; else return 1; "
        "    end; "
        "  end end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 55);
}

// ── Group 6: a user-defined exception subclass is a DIFFERENT code path ───
// A user subclass has a real TypeSymbol (declaration != nullptr), so it never
// reaches this fix's new code at all — it goes through the pre-existing
// `knownCompleteUserType` "no member `X` on type `Y` (§11.3, §12.1)" path.
// This guards that the two diagnostic paths stay correctly separated: the
// subclass's own real member (MyCode) and its inherited Message both work,
// and a bogus member gets the OLD wording, not the new §31.4 one.
BEZEL_TEST(codegen_exception_unresolved_member_2423,
           user_defined_exception_subclass_unaffected) {
    const std::string kDecl =
        "class MyError: Exception "
        "  private _myCode: int; "
        "  public constructor(msg: string, code: int) "
        "    base(msg); _myCode := code; "
        "  end constructor; "
        "  public method MyCode(): int return _myCode; end MyCode; "
        "end MyError; ";
    // Legit inherited + own member: still accepted and correct.
    BEZEL_EXPECT_EQ(run(
        "module M; " + kDecl +
        "class C public static method Main(): int "
        "  try throw new MyError(\"custom\", 7); "
        "  catch var e: MyError => return e.MyCode(); "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        7);
    // Bogus member on the subclass: rejected, but by the PRE-EXISTING
    // known-complete-user-type diagnostic (§11.3/§12.1), not this fix's new
    // §31.4 one — the two paths must not cross-fire.
    const std::string bogusProgram =
        "module M; " + kDecl +
        "class C public static method Main(): int "
        "  try throw new MyError(\"custom\", 7); "
        "  catch var e: MyError => do "
        "    var x := e.TotallyBogusOnSubclass; "
        "  end end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        bogusProgram, "no member `TotallyBogusOnSubclass` on type `MyError`"));
    BEZEL_EXPECT_TRUE(hasError(bogusProgram, "\xC2\xA7""11.3"));
}
