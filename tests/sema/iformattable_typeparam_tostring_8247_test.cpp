// Regression tests for issue #8247 — "[sema] a `T: IFormattable` receiver
// cannot call `ToString("F2")`: the call is bound to `Object.ToString` and
// rejected on arity (stdlib §2.18, §8.1, v0.355)".
//
// THE ONE ROOT CAUSE. When the compiler binds a member call on a receiver
// whose type is a TYPE PARAMETER, it read the receiver's SHAPE — "does the
// type name a declaration?" — and not the enclosing method's declared
// type-parameter CONSTRAINTS. A bare type parameter has no declaration and
// therefore no member table, so the call fell back to the §8.1 universal
// `Object` surface (`ToString(): string`, arity 0) and was then rejected with
// an ARITY complaint:
//
//   the universal `Object.ToString` member takes 0 argument(s) but the call
//   supplies 1 (§8.1)
//
// …although stdlib §2.18 / §8.1 give `IFormattable` the member
// `ToString(format: string): string`, and the body's own `where` clause
// GUARANTEES that conformance.
//
// THE AUTHORITY.
//
//   * spec §14.2 *Interface conformance* rule 6 — "T is a type parameter
//     whose constraints guarantee conformance to I: a type-bound constraint
//     naming I or an interface extending I …".
//   * spec §3.6 rule 2 — "A generic body type-checks under the weakest
//     assumption its constraints establish."
//   * stdlib §2.18 / §8.1 — `IFormattable.ToString(format: string): string`.
//
// THE SHAPE OF THE FIX. The constraint walk that reads a type parameter's
// own `where` clause now recognizes the DECLARATION-LESS `IFormattable`
// builtin interface head (stdlib §2.18; `common::kBuiltinInterfaceHeads`) —
// the same gap #7086 closed for the eight builtin COLLECTION heads — and the
// §8.1 universal-surface arity gate reads THAT verdict, so the gate and the
// member binding cannot disagree.
//
// THE ROWS.
//
//   PRIMARY rows — the generic body must COMPILE (the issue's own repro: the
//   call inside the body is what was refused), and the INTRINSIC
//   instantiation must render stdlib §2.18's own vocabulary ("F2" over a
//   `decimal` → "3.50").
//
//   CONTROL rows — the shapes the fix must NOT admit, and the shape it must
//   NOT change:
//     A. `x.ToString()` with NO argument on the same `T: IFormattable` still
//        binds to the §8.1 universal `Object.ToString` and works.
//     B. an UNCONSTRAINED `T` still cannot call `ToString("F2")` — the §8.1
//        arity refusal is unchanged.
//     C. a `T` bounded by a DIFFERENT interface (`where T: IComparable<T>`)
//        still cannot call `ToString("F2")` — the constraint must name
//        `IFormattable`, not merely some interface.
//     D. a receiver that is NOT `IFormattable` still cannot call
//        `ToString("F2")` — §2.18's vocabulary rule is unchanged.
//
// Every row is a MEASURED program (it compiles AND runs), not an assertion
// about the code.
//
// ⚠ NOT COVERED HERE, AND DELIBERATELY SO: the USER-CLASS instantiation
// (`T` at a class that implements `IFormattable` and declares its own
// `ToString(format: string)`). That leg is refused by CODEGEN, not by sema:
// the builtin `IFormattable` head carries no itable slot, so there is
// nothing for the one-argument overload to dispatch through — the gap
// `src/codegen/LlvmBackend_String.cpp:787-809` already records, and the
// §2.18 follow-up to issue #4328 (which shipped the `IDisposable` /
// `IAsyncDisposable` arms of that shape). Pinning a "not yet implemented"
// diagnostic in a regression row would freeze a gap on purpose; the commit
// body reports it instead.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::JitResult;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The rejection oracle: the named diagnostic, at a real location, and the
// compiler did not merely abort.
void expectRefused(const JitResult& r, const std::string& needle) {
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// The program's stdout, verbatim. "<did-not-run>" when the program was
// rejected, crashed or timed out.
std::string outChecked(const std::string& program) {
    auto r = runAotChecked(program);
    if (!r.ran) return std::string("<did-not-run>");
    return r.stdoutText;
}

std::string lines(std::initializer_list<std::string> parts) {
    std::string s;
    for (const auto& p : parts) {
        s += p;
        s += "\n";
    }
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// #8247 — the PRIMARY rows.
// ---------------------------------------------------------------------------

// THE ISSUE'S OWN REPRO. The generic body is what was refused: the call
// `x.ToString("F2")` over a `T: IFormattable` receiver was bound against
// the universal `Object.ToString` and rejected on arity. The body must now
// COMPILE. The method is never instantiated here, so this row measures the
// BINDING alone — exactly the population the issue filed.
BEZEL_TEST(iformattable_typeparam_tostring_8247,
           tostring_format_over_a_type_parameter_guaranteeing_iformattable_compiles) {
    BEZEL_EXPECT_EQ(
        outChecked(R"(
module M;
public class G
  public static method Show<T>(x: T): string where T: IFormattable
    return x.ToString("F2");
  end Show;
end G;
public class C
  public static method Main(): int
    Console.WriteLine("ok");
    return 0;
  end Main;
end C;
)"),
        lines({"ok"}));
}

// The INTRINSIC instantiation: `decimal` is formattable in its own right
// (stdlib §2.18's numeric vocabulary), so `ToString("F2")` over a
// `T: IFormattable` parameter instantiated at `decimal` renders "3.50".
BEZEL_TEST(iformattable_typeparam_tostring_8247,
           tostring_format_over_an_iformattable_type_parameter_at_decimal_is_admitted) {
    BEZEL_EXPECT_EQ(
        outChecked(R"(
module M;
public class G
  public static method Show<T>(x: T): string where T: IFormattable
    return x.ToString("F2");
  end Show;
end G;
public class C
  public static method Main(): int
    Console.WriteLine(G.Show(3.5));
    return 0;
  end Main;
end C;
)"),
        lines({"3.50"}));
}

// ---------------------------------------------------------------------------
// #8247 — the CONTROL rows. Every one of them must be UNCHANGED.
// ---------------------------------------------------------------------------

// CONTROL A. `x.ToString()` with NO argument is the §8.1 universal member and
// binds to it, exactly as before — the fix must not have re-bound the
// zero-argument form to `IFormattable`'s one-argument overload.
BEZEL_TEST(iformattable_typeparam_tostring_8247,
           tostring_without_a_format_over_an_iformattable_type_parameter_still_binds_to_object) {
    BEZEL_EXPECT_EQ(
        outChecked(R"(
module M;
public class G
  public static method Show<T>(x: T): string where T: IFormattable
    return x.ToString();
  end Show;
end G;
public class C
  public static method Main(): int
    Console.WriteLine(G.Show(3.5));
    return 0;
  end Main;
end C;
)"),
        lines({"3.5"}));
}

// CONTROL B. An UNCONSTRAINED type parameter has no `where` clause, so it
// establishes NOTHING (spec §3.6 rule 2) and the §8.1 universal-surface arity
// refusal stands, byte-identically.
BEZEL_TEST(iformattable_typeparam_tostring_8247,
           tostring_format_over_an_unconstrained_type_parameter_is_still_refused) {
    auto r = runAot(R"(
module M;
public class G
  public static method Show<T>(x: T): string
    return x.ToString("F2");
  end Show;
end G;
public class C
  public static method Main(): int
    Console.WriteLine(G.Show(3.5));
    return 0;
  end Main;
end C;
)");
    expectRefused(r, "the universal `Object.ToString` member takes 0 "
                      "argument(s) but the call supplies 1");
}

// CONTROL C. A `T` bounded by a DIFFERENT interface establishes no
// `IFormattable` conformance (spec §14.2 rule 6), so the refusal stands.
BEZEL_TEST(iformattable_typeparam_tostring_8247,
           tostring_format_over_a_type_parameter_bounded_by_another_interface_is_still_refused) {
    auto r = runAot(R"(
module M;
public class G
  public static method Show<T>(x: T): string where T: IComparable<T>
    return x.ToString("F2");
  end Show;
end G;
public class C
  public static method Main(): int
    Console.WriteLine(G.Show(3.5));
    return 0;
  end Main;
end C;
)");
    expectRefused(r, "the universal `Object.ToString` member takes 0 "
                      "argument(s) but the call supplies 1");
}

// CONTROL D. A receiver that is NOT `IFormattable` has no format vocabulary
// (stdlib §2.18 / spec §18.5), so its `ToString("F2")` is still refused by
// the vocabulary rule — the fix did not widen that rule either.
BEZEL_TEST(iformattable_typeparam_tostring_8247,
           tostring_format_over_a_non_iformattable_receiver_is_still_refused) {
    auto r = runAot(R"(
module M;
public class P
  public X: int;
  public constructor(x: int) X := x; end;
end P;
public class C
  public static method Main(): int
    var p := new P(1);
    Console.WriteLine(p.ToString("F2"));
    return 0;
  end Main;
end C;
)");
    expectRefused(r, "format spec requires an intrinsically-formattable operand");
}
