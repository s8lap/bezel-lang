// Regression tests for issue #8962 — §28.3 closure bullet, *Set-erasing
// positions* (v0.359): a record COMPONENT is a set-erasing position.
//
// THE SPEC.  §28.3 (v0.359), closure bullet, *Set-erasing positions*:
//
//   "... a field (instance or static ...), parameter, return slot, `ref` or
//    `out` location, **record component**, choice payload or inline-array slot
//    whose declared, pre-substitution type ... is a bare type parameter, at an
//    instantiation substituting a function type or `Object` ... — `k.Put(f)`
//    at `k: Keep<Object>` over `Put(a: T)`, **`new Box<Object>(f)` over
//    `record Box<T>(V: T)`** ... At every set-erasing position a function
//    value with a non-empty source set is rejected, and a capture-free one is
//    admitted".
//
// and, for a position declared `Object` itself: "a position declared `Object`
// (the one such type a function value converts to, §3.1)".
//
// THE DEFECT.  `scopedPreScanExpr` (src/ownership/Ownership_Checks.cpp) reads
// the constructor formals through `resolveCtorParams`, which knows a class's
// primary constructor and a choice case's payload but not a RECORD's generated
// primary constructor (§9.1), so a capture-bearing closure passed to a record
// component declared at a bare type parameter or `Object` reached no
// set-erasing check.  The closure was stored in the record, read back with the
// empty source set, and called after its capture was freed (ASan:
// heap-use-after-free; the issue's repro below).  The same gap left the
// `with(Component := f)` override of an `Object` component unchecked (§9.5:
// `with` stores into the component directly).
//
// Every reject row below is RED on the base tree (the program compiles there)
// and GREEN with the fix.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

const char* kCarriesBorrow = "function value carries captured borrow(s)";
const char* kAtBareTypeParam =
    "may not be passed where the parameter is declared at a bare type "
    "parameter";
const char* kAtObject = "may not be passed where the parameter is declared Object";

void expectSetErasingReject(const JitResult& r, const char* position) {
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kCarriesBorrow));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, position));
}

// Shared prelude: a class `Res` whose instance a closure captures, so the
// capture's source set is non-empty (§28.3 *Provenance* clause 1).
const char* kPrelude =
    "module M; "
    "import System; "
    "public class Res "
    "  public V: int; "
    "  public constructor() V := 42; end constructor; "
    "end Res; ";

std::string program(const std::string& decls, const std::string& body) {
    return std::string(kPrelude) + decls +
           " public class Program "
           "  public static method Main(): int "
           "    var r := new Res(); " +
           body +
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

}  // namespace

// The issue's own repro, verbatim: the spec's example `new Box<Object>(f)`
// over `record Box<T>(V: T)`, the closure stored through a holder and read
// back after `r` is disposed (heap-use-after-free on the base tree).
BEZEL_TEST(set_erasing_record_component_8962,
           issue_repro_generic_record_ctor_at_object_rejected) {
    auto r = runAot(
        "module M; "
        "import System; "
        "public class Res "
        "  public V: int; "
        "  public constructor() V := 42; end constructor; "
        "end Res; "
        "public record Box<T>(V: T); "
        "public class Hold "
        "  private _b: Box<Object>? := null; "
        "  public constructor() end constructor; "
        "  public method Keep(b: Box<Object>) _b := b; end Keep; "
        "  public method Run(): int "
        "    if _b <> null then "
        "      var o := _b!.V; "
        "      if o is () -> int then return (o as () -> int)!(); end if; "
        "    end if; "
        "    return 7; "
        "  end Run; "
        "end Hold; "
        "public class Program "
        "  public static method Mk(h: Hold, r: Res) "
        "    h.Keep(new Box<Object>(lambda() => r.V)); "
        "  end Mk; "
        "  public static method Main(): int "
        "    using var h := new Hold(); "
        "    for var i in 0 ..< 200 do "
        "      do "
        "        var r := new Res(); "
        "        Mk(h, r); "
        "      end; "
        "      Console.WriteLine(h.Run()); "
        "    end for; "
        "    return 0; "
        "  end Main; "
        "end Program; ");
    expectSetErasingReject(r, kAtBareTypeParam);
}

// The same component at the OTHER instantiation the bullet names: a function
// type (`b: Box<() -> int>`).  The declared, pre-substitution type is the bare
// `T` either way.
BEZEL_TEST(set_erasing_record_component_8962,
           generic_record_ctor_at_function_type_rejected) {
    auto r = runAot(program(
        "public record Box<T>(V: T); ",
        "var b := new Box<() -> int>(lambda() => r.V); "
        "Console.WriteLine(b.V()); "));
    expectSetErasingReject(r, kAtBareTypeParam);
}

// §22.5: the NAMED spelling of the component's argument binds the same
// component.
BEZEL_TEST(set_erasing_record_component_8962,
           generic_record_ctor_named_argument_rejected) {
    auto r = runAot(program(
        "public record Box<T>(V: T); ",
        "var b := new Box<Object>(V := lambda() => r.V); "
        "Console.WriteLine(1); "));
    expectSetErasingReject(r, kAtBareTypeParam);
}

// A position DECLARED `Object` (not a type parameter): a non-generic record's
// component `V: Object` is a set-erasing position on its own.
BEZEL_TEST(set_erasing_record_component_8962,
           nongeneric_object_component_ctor_rejected) {
    auto r = runAot(program(
        "public record R(V: Object); ",
        "var b := new R(lambda() => r.V); "
        "Console.WriteLine(b.V is () -> int); "));
    expectSetErasingReject(r, kAtObject);
}

// §9.5's `with` stores into the component directly, so the override of an
// `Object` component is the same position.
BEZEL_TEST(set_erasing_record_component_8962,
           with_override_of_object_component_rejected) {
    auto r = runAot(program(
        "public record R(V: Object); ",
        "var g: () -> int := lambda() => r.V; "
        "var b := new R(1); "
        "var c := b with(V := g); "
        "Console.WriteLine(c.V is () -> int); "));
    expectSetErasingReject(r, kAtObject);
}

// Boundary control (the one gtest control; the rest are in the corpus): a
// capture-FREE closure at the same position is admitted — "a capture-free one
// is admitted" — so the fix does not over-reject.
BEZEL_TEST(set_erasing_record_component_8962,
           capture_free_lambda_at_generic_record_ctor_admitted) {
    auto r = runAot(program(
        "public record Box<T>(V: T); ",
        "var b := new Box<Object>(lambda() => 5); "
        "if b.V is () -> int then Console.WriteLine(\"fn\"); end if; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "fn\n");
}
