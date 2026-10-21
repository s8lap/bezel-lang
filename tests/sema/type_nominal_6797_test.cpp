// fix #6797 (stdlib §2.13 "The `Type` class", language spec §5.1 implicit
// conversions, v0.340): the builtin `Type` is nominal. Its values come only
// from `typeof(T)` (language spec §17.10) or `GetType()` (language spec §8.2),
// so a `Type` location admits a `Type` value, or `null` for `Type?` (§7.1), and
// nothing else. The §5.1 implicit-conversion rules name no conversion into
// `Type`.
//
// The spec text, quoted from the v0.340 documents (markdown link targets omitted):
//   stdlib §2.13: "A `Type` value is obtained from the `typeof(T)` operator
//   (language spec §17.10) for a statically named type, or from
//   `value.GetType()` (language spec §8.2) for the runtime type of a value."
//   spec §7.3: "Assigning `null` to a non-nullable target is a compile error."
//
// THE DEFECT. Before the fix `var t: Type := 5;` compiled. The builtin `Type`
// had no classification, so the builtin arms of checkAssignable never fired and
// the catch-all accepted every source. An `Object` source was admitted by the
// permissive path of the Object-source arm (#6023; the remaining cells are
// #6073). Parameters, returns, fields, generic arguments and
// the Dictionary key shared the defect, and `Type?` fed an int reached malformed
// IR in codegen.
//
// These rows call parseAndCheckOwnership, which is sema only. The row that must
// reach codegen (the overload pick) lives in
// tests/codegen/type_nominal_6797_test.cpp.
//
// Each reject row asserts the nominal diagnostic at its exact line and column,
// and that it is the ONLY nominal diagnostic, so a different rejection at the
// same spot cannot satisfy it.

#include <cstddef>
#include <cstdint>
#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// The tail every nominal diagnostic shares.
const std::string kNominalTail =
    "has a type that is not implicitly convertible to `Type` (§2.13, §5.1)";

// Number of ERROR diagnostics that carry the nominal wording.
std::size_t nominalCount(const OwnershipResult& r) {
    std::size_t n = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find(kNominalTail) != std::string::npos)
            ++n;
    }
    return n;
}

// True when EXACTLY ONE nominal diagnostic exists, it begins with the position
// phrase `what`, and it sits at `line:column`.
bool nominalRejectAt(const OwnershipResult& r, const std::string& what,
                     std::uint32_t line, std::uint32_t column) {
    if (nominalCount(r) != 1) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(kNominalTail) == std::string::npos) continue;
        return d.message.rfind(what, 0) == 0 && d.location.line == line &&
               d.location.column == column;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// REJECT ROWS. A value that is not a `Type` value (§2.13) is rejected at its own
// location with the nominal diagnostic. The probes behind each row were run on
// the tip compiler; the line and column pins come from that output.
// ---------------------------------------------------------------------------

// R1: a local initialized from an int literal (stdlib §2.13, spec §5.1).
BEZEL_TEST(type_nominal_6797, local_int_initializer_rejected_spec_2_13) {
    auto r = parseAndCheckOwnership(R"bz(module M;
public class Program
  public static method Main(): int
    var t: Type := 5;
    return 0;
  end Main;
end Program;
)bz");
    BEZEL_EXPECT_TRUE(nominalRejectAt(r, "the initializer for `t`", 4, 20));
}

// R2: an `Object` source. The Object-source arm (#6023) would otherwise admit it
// into a declaration-less built-in, so the nominal rule must sit above that arm.
BEZEL_TEST(type_nominal_6797, object_source_rejected_spec_2_13) {
    auto r = parseAndCheckOwnership(R"bz(module M;
public class Program
  public static method Main(): int
    var o: Object := 5;
    var t: Type := o;
    return 0;
  end Main;
end Program;
)bz");
    BEZEL_EXPECT_TRUE(nominalRejectAt(r, "the initializer for `t`", 5, 20));
}

// R3: a value of a user class that is not named `Type`.
BEZEL_TEST(type_nominal_6797, user_class_source_rejected_spec_2_13) {
    auto r = parseAndCheckOwnership(R"bz(module M;
public class U
  public constructor() end constructor;
  destructor end destructor;
end U;
public class Program
  public static method Main(): int
    var t: Type := new U();
    return 0;
  end Main;
end Program;
)bz");
    BEZEL_EXPECT_TRUE(nominalRejectAt(r, "the initializer for `t`", 8, 20));
}

// R4: a `Type` PARAMETER reached by a call argument.
BEZEL_TEST(type_nominal_6797, call_argument_rejected_spec_2_13) {
    auto r = parseAndCheckOwnership(R"bz(module M;
public class P
  public static method Take(t: Type): int return 0; end Take;
end P;
public class Program
  public static method Main(): int
    P.Take(5);
    return 0;
  end Main;
end Program;
)bz");
    BEZEL_EXPECT_TRUE(nominalRejectAt(r, "the argument `t` to call to `Take`", 7, 12));
}

// R5: a `Type` RETURN type returning a non-`Type` value.
BEZEL_TEST(type_nominal_6797, return_value_rejected_spec_2_13) {
    auto r = parseAndCheckOwnership(R"bz(module M;
public class R
  public static method Get(): Type
    return 5;
  end Get;
end R;
)bz");
    BEZEL_EXPECT_TRUE(nominalRejectAt(r, "the returned value", 4, 12));
}

// R6: a `Type` FIELD initializer.
BEZEL_TEST(type_nominal_6797, field_initializer_rejected_spec_2_13) {
    auto r = parseAndCheckOwnership(R"bz(module M;
public class F
  public _f: Type := 5;
  public constructor() end constructor;
  destructor end destructor;
end F;
)bz");
    BEZEL_EXPECT_TRUE(nominalRejectAt(r, "the initializer for field `_f`", 3, 22));
}

// R7: a store into a `Type` field.
BEZEL_TEST(type_nominal_6797, field_assignment_rejected_spec_2_13) {
    auto r = parseAndCheckOwnership(R"bz(module M;
public class F
  public _f: Type;
  public constructor() end constructor;
  destructor end destructor;
end F;
public class Program
  public static method Main(): int
    var f := new F();
    f._f := 5;
    return 0;
  end Main;
end Program;
)bz");
    BEZEL_EXPECT_TRUE(nominalRejectAt(r, "the assigned value", 10, 13));
}

// R8: a GENERIC ARGUMENT `Type` reached through a constructor parameter `T`.
BEZEL_TEST(type_nominal_6797, generic_constructor_argument_rejected_spec_2_13) {
    auto r = parseAndCheckOwnership(R"bz(module M;
public class Box<T>
  public _v: T;
  public constructor(v: T) _v := v; end constructor;
  destructor end destructor;
end Box;
public class Program
  public static method Main(): int
    var b: Box<Type> := new Box<Type>(5);
    return 0;
  end Main;
end Program;
)bz");
    BEZEL_EXPECT_TRUE(nominalRejectAt(r, "the argument `v` to `new Box`", 9, 39));
}

// R9: a GENERIC ARGUMENT `Type` reached through a method parameter `T`.
BEZEL_TEST(type_nominal_6797, generic_method_argument_rejected_spec_2_13) {
    auto r = parseAndCheckOwnership(R"bz(module M;
public class Box<T>
  public _v: T;
  public constructor() end constructor;
  destructor end destructor;
  public method Put(v: T): () _v := v; return; end Put;
end Box;
public class Program
  public static method Main(): int
    var b := new Box<Type>();
    b.Put(5);
    return 0;
  end Main;
end Program;
)bz");
    BEZEL_EXPECT_TRUE(nominalRejectAt(r, "the argument `v` to call to `Put`", 11, 11));
}

// R10: the KEY argument of a `Dictionary<Type, int>`, a built-in generic whose
// key slot is the `Type` argument.
BEZEL_TEST(type_nominal_6797, dictionary_key_rejected_spec_2_13) {
    auto r = parseAndCheckOwnership(R"bz(module M;
public class Program
  public static method Main(): int
    var d := new Dictionary<Type, int>();
    d.Add(5, 1);
    return 0;
  end Main;
end Program;
)bz");
    BEZEL_EXPECT_TRUE(nominalRejectAt(r, "the argument to `Dictionary<T>.Add`", 5, 11));
}

// R11: a `Type?` target admits `null` or a `Type` value, not an int (spec §7.1).
BEZEL_TEST(type_nominal_6797, nullable_target_from_int_rejected_spec_2_13) {
    auto r = parseAndCheckOwnership(R"bz(module M;
public class Program
  public static method Main(): int
    var t: Type? := 5;
    return 0;
  end Main;
end Program;
)bz");
    BEZEL_EXPECT_TRUE(nominalRejectAt(r, "the initializer for `t`", 4, 21));
}

// R12: a nullable `Object?` VARIABLE is not `null`, so it is rejected for `Type?`.
BEZEL_TEST(type_nominal_6797, nullable_object_variable_rejected_spec_2_13) {
    auto r = parseAndCheckOwnership(R"bz(module M;

public class Program
  public static method Main(): int
    var o: Object? := null;
    var t: Type? := o;
    return 0;
  end Main;
end Program;
)bz");
    BEZEL_EXPECT_TRUE(nominalRejectAt(r, "the initializer for `t`", 6, 21));
}

// R13: the qualified spelling `System.Type` names the same builtin. The Core
// types row of spec §2.3 gives the builtin's module as `System`.
BEZEL_TEST(type_nominal_6797, system_qualified_spelling_rejected_spec_2_3) {
    auto r = parseAndCheckOwnership(R"bz(module M;
public class Program
  public static method Main(): int
    var t: System.Type := 5;
    return 0;
  end Main;
end Program;
)bz");
    BEZEL_EXPECT_TRUE(nominalRejectAt(r, "the initializer for `t`", 4, 27));
}

// R14: an alias of the builtin is the builtin (spec §27.8: a type declaration
// "introduces an alias for any existing type").
BEZEL_TEST(type_nominal_6797, alias_of_type_rejected_spec_27_8) {
    auto r = parseAndCheckOwnership(R"bz(module M;
type TT := Type;
public class Program
  public static method Main(): int
    var t: TT := 5;
    return 0;
  end Main;
end Program;
)bz");
    BEZEL_EXPECT_TRUE(nominalRejectAt(r, "the initializer for `t`", 5, 18));
}

// R15: a value of a TYPE PARAMETER `T` is not a `Type` value. The generic body
// is checked with `T` rigid, so this fires although `G` is never instantiated.
BEZEL_TEST(type_nominal_6797, type_parameter_value_rejected_spec_2_13) {
    auto r = parseAndCheckOwnership(R"bz(module M;
public class G<T>
  public static method Conv(x: T): int
    var t: Type := x;
    return 0;
  end Conv;
end G;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)bz");
    BEZEL_EXPECT_TRUE(nominalRejectAt(r, "the initializer for `t`", 4, 20));
}

// ---------------------------------------------------------------------------
// CONTROL ROWS. Each must keep its verdict, or keep its existing wording.
// ---------------------------------------------------------------------------

// C1: `null` into `Type?` is accepted (spec §7.3: null is valid for a nullable
// target).
BEZEL_TEST(type_nominal_6797, null_into_nullable_type_accepted_spec_7_3) {
    auto r = parseAndCheckOwnership(R"bz(module M;
public class Program
  public static method Main(): int
    var t: Type? := null;
    return 0;
  end Main;
end Program;
)bz");
    BEZEL_EXPECT_TRUE(!r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(nominalCount(r), 0u);
}

// C2: `null` into a NON-nullable `Type` keeps its existing spec §7.3 wording,
// verified on the tip compiler. It must not be re-worded as a nominal rejection.
BEZEL_TEST(type_nominal_6797, null_into_nonnullable_type_keeps_section_7_spec_7_3) {
    auto r = parseAndCheckOwnership(R"bz(module M;
public class Program
  public static method Main(): int
    var t: Type := null;
    return 0;
  end Main;
end Program;
)bz");
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "the initializer for `t` is nullable but the target type is non-nullable (§7)"));
    BEZEL_EXPECT_EQ(nominalCount(r), 0u);
}

// C3: the `typeof(T)` producer (spec §17.10) is refused by sema with the #487
// deferral, on base and on the fix alike. This row pins that the deferral is
// reported and that the nominal rule adds no second diagnostic. Because the
// producer is refused, `nominalCount == 0` here does not show that the nominal
// check ran on a `Type` value; C5 does that.
BEZEL_TEST(type_nominal_6797, typeof_into_type_deferred_spec_17_10) {
    auto r = parseAndCheckOwnership(R"bz(module M;
public class Program
  public static method Main(): int
    var t: Type := typeof(int);
    return 0;
  end Main;
end Program;
)bz");
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "reflection is not yet implemented: 'typeof' (§17.10, #487)"));
    BEZEL_EXPECT_EQ(nominalCount(r), 0u);
}

// C4: the `GetType()` producer (spec §8.2): the same deferral and the same caveat.
BEZEL_TEST(type_nominal_6797, gettype_into_type_deferred_spec_8_2) {
    auto r = parseAndCheckOwnership(R"bz(module M;
public class Program
  public static method Main(): int
    var t: Type := "x".GetType();
    return 0;
  end Main;
end Program;
)bz");
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "reflection is not yet implemented: 'GetType()' (§8.2, #487)"));
    BEZEL_EXPECT_EQ(nominalCount(r), 0u);
}

// C5: a `Type`-typed PARAMETER (a value of the builtin `Type`, not a generic type
// parameter) assigned to a `Type` local: identity, nothing to reject. This is the
// row that shows the nominal rule still admits a real `Type` value.
BEZEL_TEST(type_nominal_6797, type_valued_parameter_identity_accepted_spec_2_13) {
    auto r = parseAndCheckOwnership(R"bz(module M;
public class C
  public static method Take(t: Type): int
    var u: Type := t;
    return 0;
  end Take;
end C;
)bz");
    BEZEL_EXPECT_TRUE(!r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(nominalCount(r), 0u);
}

// C6: a nullable `Type?` local narrowed by `if n <> null then` (spec §7.4 "The
// compiler narrows local variables ... in the tracked shape below") is a `Type`
// value inside the branch.
BEZEL_TEST(type_nominal_6797, narrowed_nullable_type_accepted_spec_7_4) {
    auto r = parseAndCheckOwnership(R"bz(module M;
public class Program
  public static method Main(): int
    var n: Type? := null;
    if n <> null then
      var t: Type := n;
    end;
    return 0;
  end Main;
end Program;
)bz");
    BEZEL_EXPECT_TRUE(!r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(nominalCount(r), 0u);
}

// C7 (preserved behaviour, no new spec clause): a user class NAMED `Type` is a
// declaration, not the builtin, so its instances are assignable to it. Pinned
// existing behaviour, also covered by tests/sema/type_surface_unknown_member_5791_test.cpp.
BEZEL_TEST(type_nominal_6797, user_class_named_type_identity_preserved) {
    auto r = parseAndCheckOwnership(R"bz(module M;
public class Type
  public constructor() end constructor;
  destructor end destructor;
end Type;
public class Program
  public static method Main(): int
    var t: Type := new Type();
    return 0;
  end Main;
end Program;
)bz");
    BEZEL_EXPECT_TRUE(!r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(nominalCount(r), 0u);
}

// C8 (preserved behaviour): a mismatch against a user class NAMED `Type` is
// rejected, but not with the builtin's nominal wording.
BEZEL_TEST(type_nominal_6797, user_class_named_type_mismatch_keeps_user_wording_preserved) {
    auto r = parseAndCheckOwnership(R"bz(module M;
public class Type
  public constructor() end constructor;
  destructor end destructor;
end Type;
public class Program
  public static method Main(): int
    var u: Type := 5;
    return 0;
  end Main;
end Program;
)bz");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(nominalCount(r), 0u);
}

// C9: a generic TYPE PARAMETER named `Type` shadows the builtin (spec §2.3, Core
// types row: "a nested type or type parameter named `Object` shadows it"; this
// file reads the same clause for `Type`, which shares that row). Identity is
// accepted, and a mismatch is rejected by the type-parameter rule, never by the
// builtin's nominal rule.
BEZEL_TEST(type_nominal_6797, type_parameter_named_type_shadows_builtin_spec_2_3) {
    auto ok = parseAndCheckOwnership(R"bz(module M;
public class C
  public static method Take<Type>(t: Type): int
    var u: Type := t;
    return 0;
  end Take;
end C;
)bz");
    BEZEL_EXPECT_TRUE(!ok.diagnostics.hasErrors());

    auto bad = parseAndCheckOwnership(R"bz(module M;
public class C
  public static method Take<Type>(t: Type): int
    var u: Type := 5;
    return 0;
  end Take;
end C;
)bz");
    BEZEL_EXPECT_TRUE(hasErrorMessage(bad, "does not implicitly convert to the type parameter `Type`"));
    BEZEL_EXPECT_EQ(nominalCount(bad), 0u);
}

// C10: a `Type` value into `Object` stays accepted. The nominal rule restricts
// what may flow INTO a `Type` location, not what may leave one. Spec §5.1
// "Reference conversions": "any type to `Object`".
BEZEL_TEST(type_nominal_6797, type_value_into_object_accepted_spec_5_1) {
    auto r = parseAndCheckOwnership(R"bz(module M;
public class P
  public static method Box(t: Type): Object
    var o: Object := t;
    return o;
  end Box;
end P;
)bz");
    BEZEL_EXPECT_TRUE(!r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(nominalCount(r), 0u);
}
