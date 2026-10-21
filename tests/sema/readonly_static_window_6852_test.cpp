// Spec-derived tests for fix #6852 — §22.8 "Field initializers" and §23.1
// "Auto-properties" (v0.340): who may assign a `readonly` member, and where.
//
// The defect: a `public static readonly` member written from an UNRELATED
// class's constructor compiled and ran (`Holder.SR := 5;` in `Other`'s
// instance constructor). The static arms of the readonly write checks
// (`checkReadonlyFieldWriteTarget`, `checkPropertyWriteTarget`,
// `checkRefOutFieldOperand`, `checkRefOutPropertyOperand`) asked only whether a
// constructor, or a static constructor, was running, never whether it was the
// DECLARING type's. #5638/#5651 had already added the static-constructor and
// lambda checks to the field-assignment arm only. Two further gaps: a lambda
// body inside a constructor stayed inside the window for auto-properties (both
// assignment and ref/out) and for the ref/out operand of a field, because the
// constructor flag stays set inside a lambda; and a BARE name that resolves to
// a property was never dispatched to the ref/out property check at all.
//
// §22.8 (v0.340): a `readonly` field "may be assigned only in a field
// initializer or in a constructor body", and "The window is lexical and does
// not include a nested function body". The context is per-instance, not
// per-type: "a `static readonly` field is assignable only in its own static
// field initializer or in the declaring type's **static** constructor body,
// spelled as the bare name or `TypeName.F` — an instance constructor of the
// declaring type is not an assignment site for it".
// §23.1 (v0.340): a `readonly` auto-property "may be assigned only in a field
// initializer or in a constructor, under §22.8's per-instance assignable
// context", "admitted only through `this` for an instance auto-property, or in
// the declaring type's own static initialization for a `static readonly` one".
// §22.3 and §17.6 inherit the same window for `ref`/`out` binding and for
// `++`/`--`.
//
// Tests (REJECT rows first, ACCEPT controls last):
//   R1  §23.1 issue repro: `Holder.SR := 5` from an unrelated class's ctor
//   R2  §22.8 static readonly field `Holder.S := 5` from an unrelated static ctor
//   R3  §22.8 static readonly field, bare `S := 5`, from a derived class's static ctor
//   R4  §22.3/§22.8 static readonly field `ref` from an unrelated static ctor
//   R5  §22.3/§22.8 static readonly field `out` from an unrelated instance ctor
//   R6  §17.6/§23.1 static readonly property `++` from a nested class's ctor
//   R7  §22.8 static readonly field `:=` from the declaring type's INSTANCE ctor
//   R8  §23.1 static readonly property `:=` from the declaring type's INSTANCE ctor
//   R9  §22.3/§23.1 static readonly property passed `out` from a METHOD
//   R10 §22.3/§23.1 instance readonly property, BARE `ref X`, in an instance method
//   R11 §22.3/§23.1 instance readonly property, BARE `out X`, in a derived ctor
//       (inherited member; the declaring type is the base)
//   R12 §27.3/§22.8 instance readonly property `this.X := 5` inside a lambda
//       in its own constructor
//   R13 §27.3/§22.8 static readonly field `S := 5` inside a lambda in its own
//       static constructor
//   R14 §27.3/§22.8 instance readonly field `ref this.F` inside a lambda in
//       its own constructor
//   A1  ACCEPT: §22.8/§23.1 static readonly property written in the declaring
//       type's static ctor (bare and `Holder.SP`), read back at run time
//   A2  ACCEPT: §22.8/§22.3 static readonly field bound `ref` in its own static ctor
//   A3  ACCEPT: §22.8/§22.3 instance readonly property `ref this.X` and bare
//       `out X` in its own instance ctor
//   A4  ACCEPT: §22.3 a NON-readonly auto-property passed bare `out P` from a
//       method (the bare-name dispatch must not reject a valid program)
//   A5  ACCEPT: §22.8 a lambda inside the ctor writes a NON-readonly field
//       (the lambda exclusion is for readonly storage only)
//   A6  ACCEPT: §22.8 a derived class writes its OWN readonly field in its ctor
//   A7  ACCEPT: §22.8 a nested class's static ctor writes its OWN static readonly

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

int runProg(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

}  // namespace

// =====================================================================
// REJECT — §22.8 / §23.1 / §22.3 / §17.6: the assignable window is the
// declaring type's own (static) constructor, outside any lambda body.
// =====================================================================

// R1. The issue repro, verbatim: a `public static readonly` auto-property
// written from an unrelated class's instance constructor.
BEZEL_TEST(sema_readonly_static_window_6852,
           static_readonly_property_written_from_unrelated_ctor_rejected_spec_23_1) {
    auto r = runAot(
        "module P; "
        "public class Holder "
        "  public static readonly property SR: int := 0; "
        "  public static method Peek(): int return SR; end Peek; "
        "  destructor end destructor; "
        "end Holder; "
        "public class Other "
        "  public constructor() "
        "    Holder.SR := 5; "
        "  end constructor; "
        "  public static method Main(): int "
        "    using var o := new Other(); "
        "    return Holder.Peek(); "
        "  end Main; "
        "  destructor end destructor; "
        "end Other;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot assign to readonly property `SR`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§23.1"));
}

// R2. A `static readonly` FIELD written from an unrelated class's STATIC
// constructor: the static arm needs the declaring type, not just a ctor.
BEZEL_TEST(sema_readonly_static_window_6852,
           static_readonly_field_written_from_unrelated_static_ctor_rejected_spec_22_8) {
    auto r = runAot(
        "module M; "
        "public class Holder "
        "  public static readonly S: int; "
        "  public static method Peek(): int return S; end Peek; "
        "  destructor end destructor; "
        "end Holder; "
        "public class Other "
        "  public static constructor() "
        "    Holder.S := 5; "
        "  end constructor; "
        "  destructor end destructor; "
        "end Other; "
        "public class App "
        "  public static method Main(): int return Holder.Peek(); end Main; "
        "  destructor end destructor; "
        "end App;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot assign to readonly field `S`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§22.8"));
}

// R3. The bare spelling `S := 5` in a DERIVED class's static constructor,
// where `S` is inherited from the base: the declaring type is the base.
BEZEL_TEST(sema_readonly_static_window_6852,
           static_readonly_field_bare_write_from_derived_static_ctor_rejected_spec_22_8) {
    auto r = runAot(
        "module M; "
        "public class Base "
        "  public static readonly S: int; "
        "  public static method Peek(): int return S; end Peek; "
        "  destructor end destructor; "
        "end Base; "
        "public class Derived : Base "
        "  public static constructor() "
        "    S := 5; "
        "  end constructor; "
        "  destructor end destructor; "
        "end Derived; "
        "public class App "
        "  public static method Main(): int return Base.Peek(); end Main; "
        "  destructor end destructor; "
        "end App;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot assign to readonly field `S`"));
}

// R4. `ref` binding of a static readonly field from an unrelated static
// constructor (§22.3: the ref/out binding is the same store as `:=`).
BEZEL_TEST(sema_readonly_static_window_6852,
           static_readonly_field_ref_from_unrelated_static_ctor_rejected_spec_22_3) {
    auto r = runAot(
        "module M; "
        "public class Util "
        "  public static method Bump(ref v: int) v := v + 1; end Bump; "
        "  destructor end destructor; "
        "end Util; "
        "public class Holder "
        "  public static readonly S: int; "
        "  destructor end destructor; "
        "end Holder; "
        "public class Other "
        "  public static constructor() "
        "    Util.Bump(ref Holder.S); "
        "  end constructor; "
        "  destructor end destructor; "
        "end Other; "
        "public class App "
        "  public static method Main(): int return 0; end Main; "
        "  destructor end destructor; "
        "end App;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "a `ref` argument may not be readonly field `S`"));
}

// R5. `out` binding of a static readonly field from an unrelated INSTANCE
// constructor: the static `out` arm used to accept any constructor.
BEZEL_TEST(sema_readonly_static_window_6852,
           static_readonly_field_out_from_unrelated_instance_ctor_rejected_spec_22_3) {
    auto r = runAot(
        "module M; "
        "public class Util "
        "  public static method Fill(out v: int) v := 7; end Fill; "
        "  destructor end destructor; "
        "end Util; "
        "public class Holder "
        "  public static readonly S: int; "
        "  destructor end destructor; "
        "end Holder; "
        "public class Other "
        "  public constructor() "
        "    Util.Fill(out Holder.S); "
        "  end constructor; "
        "  destructor end destructor; "
        "end Other; "
        "public class App "
        "  public static method Main(): int return 0; end Main; "
        "  destructor end destructor; "
        "end App;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "a `out` argument may not be readonly field `S`"));
}

// R6. `++` on a static readonly auto-property from a NESTED class's
// constructor: the nested type is not the declaring type (§17.6 / §23.1).
BEZEL_TEST(sema_readonly_static_window_6852,
           static_readonly_property_increment_from_nested_ctor_rejected_spec_17_6) {
    auto r = runAot(
        "module M; "
        "public class Holder "
        "  public static readonly property SP: int := 0; "
        "  public class Inner "
        "    public constructor() "
        "      Holder.SP++; "
        "    end constructor; "
        "    destructor end destructor; "
        "  end Inner; "
        "  destructor end destructor; "
        "end Holder; "
        "public class App "
        "  public static method Main(): int return 0; end Main; "
        "  destructor end destructor; "
        "end App;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot increment/decrement readonly property `SP`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§17.6"));
}

// R7. A static readonly FIELD assigned from the declaring type's INSTANCE
// constructor: "an instance constructor of the declaring type is not an
// assignment site for it" (§22.8). Pin: base already rejects this via #5638's
// inStaticMember_ arm, so it is not a fail-without row.
BEZEL_TEST(sema_readonly_static_window_6852,
           static_readonly_field_assigned_from_declaring_instance_ctor_rejected_spec_22_8) {
    auto r = runAot(
        "module M; "
        "public class Holder "
        "  public static readonly S: int; "
        "  public constructor() "
        "    S := 5; "
        "  end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "public class App "
        "  public static method Main(): int return 0; end Main; "
        "  destructor end destructor; "
        "end App;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot assign to readonly field `S`"));
}

// R8. The same rule for a static readonly AUTO-PROPERTY: the declaring
// type's instance constructor is not its static initialization (§23.1).
BEZEL_TEST(sema_readonly_static_window_6852,
           static_readonly_property_assigned_from_declaring_instance_ctor_rejected_spec_23_1) {
    auto r = runAot(
        "module M; "
        "public class Holder "
        "  public static readonly property SP: int := 0; "
        "  public constructor() "
        "    SP := 5; "
        "  end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "public class App "
        "  public static method Main(): int return 0; end Main; "
        "  destructor end destructor; "
        "end App;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot assign to readonly property `SP`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§23.1"));
}

// R9. A static readonly auto-property passed `out` from an ordinary METHOD
// of its own class: the window is a constructor window, not a method.
BEZEL_TEST(sema_readonly_static_window_6852,
           static_readonly_property_out_from_method_rejected_spec_22_3_and_23_1) {
    auto r = runAot(
        "module M; "
        "public class Util "
        "  public static method Fill(out v: int) v := 7; end Fill; "
        "  destructor end destructor; "
        "end Util; "
        "public class Holder "
        "  public static readonly property SP: int := 0; "
        "  public static method Mut() "
        "    Util.Fill(out SP); "
        "  end Mut; "
        "  destructor end destructor; "
        "end Holder; "
        "public class App "
        "  public static method Main(): int return 0; end Main; "
        "  destructor end destructor; "
        "end App;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "a `out` argument must be a property with an accessible setter"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§23.1"));
}

// R10. BARE `ref X` for an instance readonly auto-property in an instance
// method. The bare spelling is the same store as `this.X`, which was already
// rejected; the bare name was never checked (the dispatch gap).
BEZEL_TEST(sema_readonly_static_window_6852,
           instance_readonly_property_bare_ref_in_method_rejected_spec_22_3_and_23_1) {
    auto r = runAot(
        "module M; "
        "public class Util "
        "  public static method Bump(ref v: int) v := v + 1; end Bump; "
        "  destructor end destructor; "
        "end Util; "
        "public class Holder "
        "  public readonly property X: int := 0; "
        "  public method Mut() "
        "    Util.Bump(ref X); "
        "  end Mut; "
        "  destructor end destructor; "
        "end Holder; "
        "public class App "
        "  public static method Main(): int return 0; end Main; "
        "  destructor end destructor; "
        "end App;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "a `ref` argument must be a property with an accessible setter"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§23.1"));
}

// R11. BARE `out X` for an INHERITED instance readonly auto-property in a
// derived class's constructor: the declaring type is the base (§22.8).
BEZEL_TEST(sema_readonly_static_window_6852,
           inherited_readonly_property_bare_out_from_derived_ctor_rejected_spec_22_8_and_23_1) {
    auto r = runAot(
        "module M; "
        "public class Util "
        "  public static method Fill(out v: int) v := 7; end Fill; "
        "  destructor end destructor; "
        "end Util; "
        "public class Base "
        "  public readonly property X: int := 0; "
        "  destructor end destructor; "
        "end Base; "
        "public class Derived : Base "
        "  public constructor() "
        "    Util.Fill(out X); "
        "  end constructor; "
        "  destructor end destructor; "
        "end Derived; "
        "public class App "
        "  public static method Main(): int return 0; end Main; "
        "  destructor end destructor; "
        "end App;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "a `out` argument must be a property with an accessible setter"));
}

// R12. The lambda window (§22.8): `this.X := 5` for an instance readonly
// auto-property inside a lambda written in its own constructor is outside
// every assignment window.
BEZEL_TEST(sema_readonly_static_window_6852,
           instance_readonly_property_assigned_in_ctor_lambda_rejected_spec_22_8_and_23_1) {
    auto r = runAot(
        "module M; "
        "public class Holder "
        "  public readonly property X: int := 0; "
        "  public constructor() "
        "    var f := lambda() => do this.X := 5; end; "
        "    f(); "
        "  end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "public class App "
        "  public static method Main(): int return 0; end Main; "
        "  destructor end destructor; "
        "end App;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot assign to readonly property `X`"));
}

// R13. The lambda window for a STATIC readonly field, written inside a lambda
// in the declaring type's own static constructor. Pin: base already rejects this
// via #5638's !inLambda_ arm, so it is not a fail-without row.
BEZEL_TEST(sema_readonly_static_window_6852,
           static_readonly_field_assigned_in_static_ctor_lambda_rejected_spec_22_8) {
    auto r = runAot(
        "module M; "
        "public class Holder "
        "  public static readonly S: int; "
        "  public static constructor() "
        "    var f := lambda() => do S := 5; end; "
        "    f(); "
        "  end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "public class App "
        "  public static method Main(): int return 0; end Main; "
        "  destructor end destructor; "
        "end App;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot assign to readonly field `S`"));
}

// R14. `ref this.F` for an instance readonly field inside a lambda in its own
// constructor: §22.3 inherits the §22.8 lambda exclusion for ref/out binding.
BEZEL_TEST(sema_readonly_static_window_6852,
           instance_readonly_field_ref_in_ctor_lambda_rejected_spec_22_3_and_22_8) {
    auto r = runAot(
        "module M; "
        "public class Util "
        "  public static method Bump(ref v: int) v := v + 1; end Bump; "
        "  destructor end destructor; "
        "end Util; "
        "public class Holder "
        "  public readonly F: int; "
        "  public constructor() "
        "    var f := lambda() => do Util.Bump(ref this.F); end; "
        "    f(); "
        "  end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "public class App "
        "  public static method Main(): int return 0; end Main; "
        "  destructor end destructor; "
        "end App;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "a `ref` argument may not be readonly field `F`"));
}

// =====================================================================
// ACCEPT — the controls that must keep compiling and running.
// =====================================================================

// A1. The declaring type's own static constructor writes its static readonly
// auto-property, bare and `Holder.SQ`, and the values are read back at run time.
BEZEL_TEST(sema_readonly_static_window_6852,
           static_readonly_property_written_in_declaring_static_ctor_accepted_spec_23_1) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public class Holder "
        "  public static readonly property SP: int := 0; "
        "  public static readonly property SQ: int := 0; "
        "  public static constructor() "
        "    SP := 5; "
        "    Holder.SQ := 7; "
        "  end constructor; "
        "  public static method Get(): int return SP + SQ; end Get; "
        "  destructor end destructor; "
        "end Holder; "
        "public class App "
        "  public static method Main(): int return Holder.Get(); end Main; "
        "  destructor end destructor; "
        "end App;"),
        12);
}

// A2. A `ref` binding of the declaring type's own static readonly field inside
// its own static constructor is the same store as `:=` and is admitted by the
// same window (§22.3 inherits §22.8's context). The binding is the field's
// single assignment (§22.8 single-assignment), so there is no `:=` beside it;
// the default 0 is bumped once to 1.
BEZEL_TEST(sema_readonly_static_window_6852,
           static_readonly_field_ref_in_declaring_static_ctor_accepted_spec_22_3_and_22_8) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public class Util "
        "  public static method Bump(ref v: int) v := v + 1; end Bump; "
        "  destructor end destructor; "
        "end Util; "
        "public class Holder "
        "  public static readonly S: int; "
        "  public static constructor() "
        "    Util.Bump(ref S); "
        "  end constructor; "
        "  public static method Get(): int return S; end Get; "
        "  destructor end destructor; "
        "end Holder; "
        "public class App "
        "  public static method Main(): int return Holder.Get(); end Main; "
        "  destructor end destructor; "
        "end App;"),
        1);
}

// A3. An instance readonly auto-property passed `ref` as `this.X` and `out` by
// its bare name, both inside its own instance constructor: the two admitted
// spellings of §23.1 (`this`-rooted, and the bare in-scope spelling).
BEZEL_TEST(sema_readonly_static_window_6852,
           instance_readonly_property_ref_this_and_bare_out_in_own_ctor_accepted_spec_22_3_and_23_1) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public class Util "
        "  public static method Bump(ref v: int) v := v + 1; end Bump; "
        "  public static method Fill(out v: int) v := 7; end Fill; "
        "  destructor end destructor; "
        "end Util; "
        "public class Holder "
        "  public readonly property X: int := 0; "
        "  public readonly property Y: int := 0; "
        "  public constructor() "
        "    Util.Bump(ref this.X); "
        "    Util.Fill(out Y); "
        "  end constructor; "
        "  public method Sum(): int return X + Y; end Sum; "
        "  destructor end destructor; "
        "end Holder; "
        "public class App "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    return h.Sum(); "
        "  end Main; "
        "  destructor end destructor; "
        "end App;"),
        8);
}

// A4. A NON-readonly auto-property passed bare `out P` from an ordinary method:
// the bare-name ref/out dispatch added for #6852 must leave writable properties
// alone (their setter is accessible everywhere).
BEZEL_TEST(sema_readonly_static_window_6852,
           non_readonly_property_bare_out_from_method_accepted_spec_22_3) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public class Util "
        "  public static method Fill(out v: int) v := 7; end Fill; "
        "  destructor end destructor; "
        "end Util; "
        "public class Holder "
        "  public property P: int := 0; "
        "  public method Mut(): int "
        "    Util.Fill(out P); "
        "    return P; "
        "  end Mut; "
        "  destructor end destructor; "
        "end Holder; "
        "public class App "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    return h.Mut(); "
        "  end Main; "
        "  destructor end destructor; "
        "end App;"),
        7);
}

// A5. The lambda window (§22.8) excludes only READONLY storage: a lambda in a
// constructor that writes a non-readonly auto-property is accepted, and the
// write is observed after the constructor returns.
BEZEL_TEST(sema_readonly_static_window_6852,
           non_readonly_property_written_in_ctor_lambda_accepted_spec_22_8) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public class Holder "
        "  public property P: int := 0; "
        "  public constructor() "
        "    var f := lambda() => do P := 5; end; "
        "    f(); "
        "  end constructor; "
        "  public method Get(): int return P; end Get; "
        "  destructor end destructor; "
        "end Holder; "
        "public class App "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    return h.Get(); "
        "  end Main; "
        "  destructor end destructor; "
        "end App;"),
        5);
}

// A6. A derived class writes a readonly field it DECLARES, in its own
// constructor: the declaring type is the derived class, so the window is open.
BEZEL_TEST(sema_readonly_static_window_6852,
           derived_class_writes_its_own_readonly_field_in_ctor_accepted_spec_22_8) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public class Base "
        "  destructor end destructor; "
        "end Base; "
        "public class Derived : Base "
        "  public readonly D: int; "
        "  public constructor() "
        "    D := 2; "
        "  end constructor; "
        "  public method Get(): int return D; end Get; "
        "  destructor end destructor; "
        "end Derived; "
        "public class App "
        "  public static method Main(): int "
        "    using var d := new Derived(); "
        "    return d.Get(); "
        "  end Main; "
        "  destructor end destructor; "
        "end App;"),
        2);
}

// A7. A nested class's static constructor writes a static readonly field that
// the NESTED class declares: the declaring type is the nested class itself.
BEZEL_TEST(sema_readonly_static_window_6852,
           nested_class_static_ctor_writes_its_own_static_readonly_accepted_spec_22_8) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public class Outer "
        "  public class Inner "
        "    public static readonly S: int; "
        "    public static constructor() "
        "      S := 9; "
        "    end constructor; "
        "    public static method Get(): int return S; end Get; "
        "    destructor end destructor; "
        "  end Inner; "
        "  destructor end destructor; "
        "end Outer; "
        "public class App "
        "  public static method Main(): int return Outer.Inner.Get(); end Main; "
        "  destructor end destructor; "
        "end App;"),
        9);
}
