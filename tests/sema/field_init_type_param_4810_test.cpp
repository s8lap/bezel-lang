// Regression tests for issue #4810 — a primitive literal initializer into a
// TYPE-PARAMETER-TYPED FIELD (record body-field / property, or class instance
// field) was silently accepted by `checkAssignable`'s catch-all.
//
// MEASURED (issue body): `public dv: U := 99` on a generic record / class
// compiles clean; at `U = string` the runtime reinterpreted the int payload
// as a string header (same defect class as #4768's forward direction);
// `U = SomeClass` would silently store the int where a reference was expected.
//
// Spec (v0.307): §5.1's conversion table does not list a primitive value
// being implicitly convertible to a TYPE PARAMETER — the only conversions
// out of a type parameter are "to an interface or class its constraints
// guarantee conformance to" (plus `Object`), and constant narrowing requires
// "a written integer target" (§5.1, constant-narrowing paragraph). A generic
// field declared at an unconstrained `U` has no such target, so a primitive
// literal initializer has no §5.1 conversion path. §3.6 rule 2 reinforces: a
// generic body type-checks under the weakest assumption its constraints
// establish, and no constraint names a primitive.
//
// This is the FORWARD direction of #4768: #4768 rejected `var t: int := v`
// where `v: T` (type-param SOURCE, primitive TARGET). #4810 rejects the
// mirror: `public f: T := 99` (primitive SOURCE, type-param TARGET).
//
// ORACLE. Reject rows assert rejection AND the arm's own wording (unique in
// src/). Accept controls RUN and assert exact stdout.
//
// Scope: per issue body — UNCONSTRAINED type parameters only. A CONSTRAINED
// parameter like `where T: value` admits primitive substitutions and §14.2
// defers the conversion check to instantiation ("operations that copy T …
// are checked at instantiation"); the fix's narrow issuance respects that
// pin (one of the rows below makes this explicit).
//
// Cite: spec §5.1 (implicit conversions, constant narrowing), §14.2
// (constraints — unconstrained type-parameter-typed storage has no
// primitive guarantee; constrained storage defers to instantiation),
// v0.307. Related: #4768 (reverse direction), #3944 (the same generic
// body-field default at the codegen side, with different failure surface).

#include "support/aot_support.hpp"  // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

// Distinctive substring of this fix's diagnostic — the source-side mirror of
// #4768's wording. The arm names the conversion's missing target/permission
// and points at the legal alternative (`default(T)` or a constrained bound).
constexpr const char* kRuleWording =
    "is a type parameter, and a primitive literal cannot initialize a "
    "generic field whose type parameter may resolve to a class, record, "
    "or non-int primitive";

bool rejectedByRule(const std::string& program, const char* paramName) {
    auto r = runAot(program);
    if (r.ran) return false;
    const std::string head = std::string("`") + paramName + "` ";
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(head + kRuleWording) != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// ═════════ Reject rows ════════════════════════════════════════════════════

// The issue's headline repro: a class instance field `public dv: U := 99`,
// where `U` is an unconstrained type parameter of the enclosing class.
BEZEL_TEST(field_init_type_param_4810,
           class_field_primitive_into_type_param_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public class Holder<U>
  public dv: U := 99;
  public static method Main(): int
    return 0;
  end Main;
end Holder;
)BZ", "U"));
}

// Same shape on a generic RECORD body-field (positional record variant uses
// `public F: T := 7` — this is the test #3944 already exercised, but #3944
// pins the OLD acceptance; here we assert the REJECT — same defect, but
// pinned at sema-time, not codegen-time).
BEZEL_TEST(field_init_type_param_4810,
           record_body_field_primitive_into_type_param_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public record Box<T>(X: T)
  public F: T := 7;
end Box;
public class Program
  public static method Main(): int
    var b := new Box<int>(3);
    return b.F;
  end Main;
end Program;
)BZ", "T"));
}

// Same shape on a record auto-property (the §9.2 readonly auto-property form
// — the #3944 codegen-side row's sema equivalent).
BEZEL_TEST(field_init_type_param_4810,
           record_auto_property_primitive_into_type_param_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public record Box<T>(X: T)
  public readonly property D: T := 5;
end Box;
public class Program
  public static method Main(): int
    var b := new Box<int>(3);
    return b.D;
  end Main;
end Program;
)BZ", "T"));
}

// Method-level type parameter: a method-local field (records have them as
// record-positional; here a `var` local at a `U`-typed annotation with a
// primitive initializer — same direction as the field-init case).
BEZEL_TEST(field_init_type_param_4810,
           local_var_primitive_into_method_type_param_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByRule(R"BZ(
module Conf;
public class Holder
  public static method Make<U>(): U
    var v: U := 99;
    return v;
  end Make;
end Holder;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)BZ", "U"));
}

// Constraint DOES relax the rule: `where T: value` admits primitive
// substitutions (alongside records / enums / choices / inline arrays),
// and §14.2's instantiation-time check (§14.2: "operations that copy T …
// are checked at instantiation") handles a primitive-literal initializer
// into T-typed storage as a deferred-to-instantiation conversion.
// THIS IS THE PINNED ACCEPTANCE SHAPE — the same shape
// `generic_ref_out_exact_unification_1686__…` exercises — and the fix's
// narrow issuance MUST NOT reject it. Sanity control: this rows RUNS,
// prints `99`, and exercises the FieldDecl + checkAssignable paths.
BEZEL_TEST(field_init_type_param_4810,
           constrained_type_param_field_primitive_deferred_spec_14_2) {
    auto r = runAot(R"BZ(
module Conf;
public class Holder<U> where U: value
  public dv: U := 99;
  public static method Main(): int
    var h := new Holder<int>();
    Console.WriteLine(h.dv);
    return 0;
  end Main;
end Holder;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("99\n"));
}

// ═════════ Accept controls ═════════════════════════════════════════════════

// The legal spelling: `public dv: U := default(U)` — `default(U)` returns
// `U` for value-type U and `U?` for class U, always compatible with `U`
// (§17.11 The default operator, §14.2). This MUST remain accepted; without
// it, every existing generic record body-field default that uses `default(T)`
// breaks (#3944's own codegen shape, if rewritten to use `default(T)`).
BEZEL_TEST(field_init_type_param_4810,
           default_typed_expression_into_type_param_field_accepted_spec_17_11) {
    auto r = runAot(R"BZ(
module Conf;
public record Box<T>(X: T)
  public F: T := default(T);
end Box;
public class Program
  public static method Main(): int
    var b := new Box<int>(3);
    Console.WriteLine(b.F);
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// Concrete-primitive target accepts a primitive initializer at the SAME
// generic position — sanity control: this is the case §5.1's constant
// narrowing admits (`var b: byte := 50;`) and it must NOT be touched by the
// new arm.
BEZEL_TEST(field_init_type_param_4810,
           concrete_int_field_primitive_init_still_accepted_spec_5_1) {
    auto r = runAot(R"BZ(
module Conf;
public record Box<T>(X: T)
  public F: int := 5;
end Box;
public class Program
  public static method Main(): int
    var b := new Box<int>(3);
    Console.WriteLine(b.F);
    return 0;
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}