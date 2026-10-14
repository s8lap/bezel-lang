// Spec-derived tests for #6146 — §35.11 "Generics" and §28.3 "Local variables,
// ownership, and borrows" (v0.338).
//
// Defect: the type of a payload-FREE case of an explicitly instantiated
// generic choice lost its type arguments. `Opt<int>.No` was typed as the bare
// `Opt`, whose `owned T` payload reads as a class-assumption owned marker, so
// the choice was classified owning and `var b := a; var c := a;` reported
// "owned value in `a` was already moved out (§28.3)". The payload CALL form
// (`Opt<int>.Has(7)`) already copied the instantiation's type arguments from
// the target; the bare-case arms in resolveMemberAccess did not.
//
// Spec rules (v0.338):
//   §35.11: "a choice with `Ok(owned value: T)` is value-like at `T = int`
//   (marker erased, payload an admissible value) and owning at `T = Connection`
//   (marker means owned)". Classification is per instantiation.
//   §35.4 rule 1: a choice is owning if a case has an `owned` payload that is a
//   class, interface, or — at a class or interface instantiation — bare type-
//   parameter payload. Rule 3: otherwise value-like, and "a value-like choice
//   is an ordinary copyable value type". Payloads of runtime-managed type
//   (`string`) are value payloads under rule 3.
//   §28.1 clause 3: at every other value or runtime-managed substitution the
//   `owned` marker over a bare type parameter is erased.
//   §28.3: a move-only (owning) value is moved out by a local-to-local copy,
//   and a later use is "already moved out".
//
// Consequence pinned here: `Opt<int>.No` and `Opt<string>.No` copy freely;
// `Opt<Conn>.No` (class payload instantiation) and `Opt<Opt<Conn>>.No` still
// move.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

}  // namespace

// =====================================================================
// ACCEPT — value-like instantiations copy freely. Checked oracle (ASan on,
// leak detection on); stdout asserted, since the exit code is 8-bit truncated.
// =====================================================================

// §35.11 / §28.3 headline repro from #6146, with an observable result.
BEZEL_TEST(choice_payloadless_case_type_arg_6146,
           payloadless_case_int_copied_twice_spec_35_11_28_3) {
    auto r = runAotChecked(
        "module M; "
        "choice Opt<T> Has(owned v: T), No; end Opt; "
        "class Program public static method Main(): int "
        "  var a := Opt<int>.No; "
        "  var b := a; "
        "  var c := a; "
        "  Console.WriteLine(1); "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// §35.11: the same value passed by value to a function twice (a copy at each
// call site).
BEZEL_TEST(choice_payloadless_case_type_arg_6146,
           payloadless_case_int_passed_to_function_twice_spec_35_11) {
    auto r = runAotChecked(
        "module M; "
        "choice Opt<T> Has(owned v: T), No; end Opt; "
        "class Program "
        "  public static method Take(v: Opt<int>): int return 1; end Take; "
        "  public static method Main(): int "
        "    var a := Opt<int>.No; "
        "    var s := Take(a); "
        "    var t := Take(a); "
        "    Console.WriteLine(s + t); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

// §35.11: a value-like instantiation stored in a record field.
BEZEL_TEST(choice_payloadless_case_type_arg_6146,
           payloadless_case_int_stored_in_record_field_spec_35_11) {
    auto r = runAotChecked(
        "module M; "
        "choice Opt<T> Has(owned v: T), No; end Opt; "
        "public record Holder(V: Opt<int>) end Holder; "
        "class Program public static method Main(): int "
        "  var a := Opt<int>.No; "
        "  var h := new Holder(a); "
        "  var b := a; "
        "  Console.WriteLine(4); "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\n"));
}

// §35.11: a generic method returns `Opt<T>.No`; at T = int the result is a
// value-like choice and copies freely.
BEZEL_TEST(choice_payloadless_case_type_arg_6146,
           generic_method_returning_payloadless_case_spec_35_11) {
    auto r = runAotChecked(
        "module M; "
        "choice Opt<T> Has(owned v: T), No; end Opt; "
        "class Program "
        "  public static method Nothing<T>(): Opt<T> "
        "    return Opt<T>.No; "
        "  end Nothing; "
        "  public static method Main(): int "
        "    var a := Nothing<int>(); "
        "    var b := a; "
        "    var c := a; "
        "    Console.WriteLine(2); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

// §35.11: `Opt<Opt<int>>` — the outer instantiation's payload is itself a
// value-like choice, so the outer is value-like too (rule 3, transitively).
BEZEL_TEST(choice_payloadless_case_type_arg_6146,
           nested_generic_choice_payloadless_case_spec_35_11) {
    auto r = runAotChecked(
        "module M; "
        "choice Opt<T> Has(owned v: T), No; end Opt; "
        "class Program public static method Main(): int "
        "  var a := Opt<Opt<int>>.No; "
        "  var b := a; "
        "  var c := a; "
        "  Console.WriteLine(3); "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

// §35.11 / §35.4 rule 3: `string` is a runtime-managed value payload. At
// T = string the `owned` marker is erased by §28.1 clause 3, so the choice is
// value-like and `Opt<string>.No` copies freely. (Pinned per the spec text;
// see the handback note on the brief's contrary expectation.)
BEZEL_TEST(choice_payloadless_case_type_arg_6146,
           payloadless_case_string_copied_twice_spec_35_4_28_1) {
    auto r = runAotChecked(
        "module M; "
        "choice Opt<T> Has(owned v: T), No; end Opt; "
        "class Program public static method Main(): int "
        "  var a := Opt<string>.No; "
        "  var b := a; "
        "  var c := a; "
        "  Console.WriteLine(5); "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}

// =====================================================================
// REJECT — owning instantiations still move. The NEWLY-ACCEPTED-INVALID
// guard: the instantiation's type arguments must not turn a class payload
// into a value.
// =====================================================================

// §35.4 rule 1 / §28.3: `Opt<Conn>` is owning (marker means owned at a class
// instantiation), so the payloadless copy moves `a` and the second copy is a
// use-after-move.
BEZEL_TEST(choice_payloadless_case_type_arg_6146,
           payloadless_case_class_instantiation_still_moves_spec_35_4_28_3) {
    auto r = runAot(
        "module M; "
        "class Conn end Conn; "
        "choice Opt<T> Has(owned v: T), No; end Opt; "
        "class Program public static method Main(): int "
        "  var a := Opt<Conn>.No; "
        "  var b := a; "
        "  var c := a; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "owned value in `a` was already moved out at this point (§28.3)"));
}

// §35.4 rule 1: a choice with a CONCRETE class payload is owning in every
// case — no instantiation to erase against — so the bare payloadless copy
// still moves. Unchanged by this fix; pinned so it cannot regress.
BEZEL_TEST(choice_payloadless_case_type_arg_6146,
           concrete_owned_class_choice_payloadless_still_moves_spec_35_4) {
    auto r = runAot(
        "module M; "
        "class Conn end Conn; "
        "choice H Has(owned c: Conn), No; end H; "
        "class Program public static method Main(): int "
        "  var a := H.No; "
        "  var b := a; "
        "  var c := a; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "owned value in `a` was already moved out at this point (§28.3)"));
}

// §35.4 rule 1 at the outer level: `Opt<Opt<Conn>>` holds an owning payload
// transitively, so it is owning and still moves.
BEZEL_TEST(choice_payloadless_case_type_arg_6146,
           nested_class_instantiation_payloadless_still_moves_spec_35_4) {
    auto r = runAot(
        "module M; "
        "class Conn end Conn; "
        "choice Opt<T> Has(owned v: T), No; end Opt; "
        "class Program public static method Main(): int "
        "  var a := Opt<Opt<Conn>>.No; "
        "  var b := a; "
        "  var c := a; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "owned value in `a` was already moved out at this point (§28.3)"));
}

// §28.1 clause 3 / §35.11: a generic method returning `Opt<T>.No` at
// T = Conn yields an owning value; the result still moves.
BEZEL_TEST(choice_payloadless_case_type_arg_6146,
           generic_method_payloadless_class_instantiation_still_moves_spec_28_1) {
    auto r = runAot(
        "module M; "
        "class Conn end Conn; "
        "choice Opt<T> Has(owned v: T), No; end Opt; "
        "class Program "
        "  public static method Nothing<T>(): Opt<T> "
        "    return Opt<T>.No; "
        "  end Nothing; "
        "  public static method Main(): int "
        "    var a := Nothing<Conn>(); "
        "    var b := a; "
        "    var c := a; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "owned value in `a` was already moved out at this point (§28.3)"));
}
