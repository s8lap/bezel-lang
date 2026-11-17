// Regression tests for issue #8639 — `checkAssignable` admitted a CONCRETE
// PRIMITIVE into a type-parameter target whenever the parameter carried a
// `where` constraint.
//
// The issue's repro (silent acceptance):
//
//   public static method F<T>(x: T): T where T: notnull
//     var y: T := 5;   // accepted, no diagnostic
//     return y;
//   end F;
//
// SPEC (v0.357). §5.1 "Implicit conversions" lists the implicit conversions,
// and its reference-conversion list names a type parameter ONLY as the SOURCE
// of a conversion:
//
//   "**Reference conversions.** The following conversions are implicit …
//    - identity: every type converts to itself;
//    - class to base class, transitively, up to `Object`;
//    - **any type to `Object`** …
//    - a type to an interface it implements or conforms to …
//    - interface to extended interface, directly or transitively;
//    - **a type parameter to an interface or class its constraints guarantee
//      conformance to** (§14.2) …
//    - the variance-induced conversions of §12.9 …"
//
// There is no row whose SOURCE is a concrete type and whose TARGET is a type
// parameter, so `int` → `T` has no §5.1 conversion path. The other two
// candidate paths fail too: constant narrowing needs "any integer type whose
// range contains its value" (§5.1) and a type parameter is not an integer
// type, and the numeric table widens to a CONCRETE integer type.
//
// §14.2's constraint table states what substitutions a constraint ADMITS
// (`notnull` admits "Non-nullable types only (value or reference)"; `value`
// admits "records, enums, choices …, primitives …, the unit type"; `class`
// admits "Non-nullable reference types only"; an interface bound admits the
// conforming types; a class bound admits "Non-nullable subtypes"). A
// constraint ADMITS substitutions; it does not create a conversion. §3.6
// rule 2 then decides where the initializer is checked: "A generic body
// type-checks under the weakest assumption its constraints establish" — so
// the initializer must be assignable to EVERY admitted substitution, and `int`
// is not one of them under `notnull` (a `string` is admitted too), nor under
// `value` (a record is admitted too), nor under `class` (no primitive is a
// class), nor under an interface or class bound (a conforming class is
// admitted too). §14.2 *Dispatched generic methods* states the same rule for
// the dispatched case: "Requirements are decided at the body … a requirement
// of its own type parameter that the set does not establish is a **compile
// error at that body**".
//
// The constraint does NOT defer this check to the instantiation. §7.10's
// "operations that copy T … are checked at instantiation" is the COPY DEMAND
// (§7.10's next paragraph: "That check is not the `value` constraint's: it is
// a requirement of the body's use of any type parameter whose constraints
// admit a move-only substitution"), and `var y: T := 5` is not an operation
// that copies a T — it copies an `int` INTO a T slot.
//
// The sibling arm (#5469 — a concrete CLASS or RECORD source into a type
// parameter) was never constraint-gated and already rejected that shape under
// every constraint kind; this fix makes the primitive source match it.
//
// ORACLE. Both rows assert rejection AND the diagnostic's own wording, so a
// row cannot pass on an unrelated rejection. They FAIL on the pre-fix
// compiler, where the programs compile clean.
//
// Cite: spec §5.1 (implicit conversions — the closed list, constant
// narrowing), §3.6 rule 2 (a generic body type-checks under the weakest
// assumption its constraints establish), §14.2 (constraints; *Dispatched
// generic methods* — "Requirements are decided at the body"), §7.10 (the copy
// demand), §17.11 (`default(T)`), §7.7 (the `??` right operand), v0.357.
// Related: #4810 (the unconstrained half of this arm), #5469 (the
// class/record sibling arm), #8596 (the `??` right operand's check).

#include "support/aot_support.hpp"  // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

// The GENERAL `checkAssignable` arm's wording — the #4916 split's NEW
// wording, which names the SOURCE type and cites §3.6 rule 2. Distinctive
// substring; the arm fires at every §5.1 conversion site (local var,
// argument, return, assignment, the `??` right operand).
constexpr const char* kGeneralArmWording =
    "does not implicitly convert to the type parameter";

// The #8639 constrained tail: the reason the constraint does not supply the
// conversion. A row that pinned only the lead would also pass on the
// pre-#8639 wording, so the tail is pinned too.
constexpr const char* kConstrainedTail =
    "lists no conversion from a concrete type to a type parameter";

// True when the program is REJECTED and the rejection carries the general
// arm's wording and the #8639 constrained tail.
bool rejectedByConstrainedTypeParamRule(const std::string& program) {
    auto r = runAot(program);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(kGeneralArmWording) == std::string::npos) continue;
        if (d.message.find(kConstrainedTail) == std::string::npos) continue;
        return true;
    }
    return false;
}

}  // namespace

// ═════════ Reject rows ════════════════════════════════════════════════════

// The issue's own repro: a primitive literal into a `where T: notnull`
// type-parameter target. On the pre-fix compiler this compiled clean (the
// #4810 arm was gated on the parameter being UNCONSTRAINED), so this row
// FAILS there.
BEZEL_TEST(type_param_primitive_target_8639,
           local_var_primitive_into_notnull_type_param_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedByConstrainedTypeParamRule(R"BZ(
module Conf;
public class Holder
  public static method F<T>(x: T): T where T: notnull
    var y: T := 5;
    return y;
  end F;
  public static method Main(): int
    return 0;
  end Main;
end Holder;
)BZ"));
}

// The `??` right operand of #8596: §7.7's typing for a left operand of type
// `T?` requires the right operand to be "implicitly convertible … to `T` or
// to `T?`", and the #8596 ruling extends that to a non-nullable left operand.
// The right operand is type-checked through the SAME `checkAssignable` arm, so
// it inherited #8639's acceptance. On the pre-fix compiler this compiled
// clean (only the "the right operand is never evaluated" warning was
// emitted), so this row FAILS there.
BEZEL_TEST(type_param_primitive_target_8639,
           coalesce_right_operand_primitive_into_notnull_type_param_rejected_spec_7_7) {
    BEZEL_EXPECT_TRUE(rejectedByConstrainedTypeParamRule(R"BZ(
module Conf;
public class Holder
  public static method F<T>(x: T): T where T: notnull
    var y: T := x ?? 5;
    return y;
  end F;
  public static method Main(): int
    return 0;
  end Main;
end Holder;
)BZ"));
}
