// Sema — issue #9105 (spec §28.3.5 *The announcement table* row 4, v0.359;
// §27.1.1 fourth bullet, v0.359): a function value whose RETURN slot is
// `owned` — the type of a local closure over a fresh body
// (`var f := lambda() => new K(5)` is `() -> owned K`), of a method-group
// reference to an `owned K` producer, or of any `() -> owned K` variable —
// may not be retyped to a plain concrete class/interface return slot
// `() -> K`.
//
// WHY. Row 4's *Function-value invocation* cell reads the TARGET slot:
// "Borrow, sources = … The result is never owned." So a call through a plain
// `() -> K` slot yields a borrow nobody disposes; if the function value
// behind it mints a FRESH frame per call, every call leaks it (3200 B over
// the 200 iterations of the issue's p13/p14). §27.1.1: "Modifier slots
// (`owned`/`ref`/`out`/`out owned`/`scoped`) must match exactly". #9040
// rejected the DIRECT spelling (a lambda literal minting into a plain slot);
// three routes still reached the slot as an already-formed value.
//
// ROWS. One RED row per fixed route (each RED on r30-c1 with this file's
// fix reverted: the program is admitted, `hasErrors()` is false) and two
// boundary controls, each GREEN on both arms and RED under a named wrong fix.
//
// Spec-unclear: none for these routes. §27.1.1's exact-match rule is
// broader than this fix (it also covers the opposite direction, a plain
// `() -> K` value at `() -> owned K`, the mint-announcing return slots and a
// bare-type-parameter slot); this fix is keyed on the target slot row 4
// names (`isConcreteSlotReturnType`) and leaves those untouched.

#include <string>

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::SemaResult;

namespace {

SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasMsg(const SemaResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

const char* kKlass =
    "public class K "
    "  public readonly N: int; "
    "  public constructor(n: int) N := n; end constructor; "
    "end K; ";

// Route 1 (issue p13): a returned local closure `var f := lambda() => new K(5)`.
std::string returnedLocalClosure() {
    return std::string("module M; ") + kKlass +
        "public class Program "
        "  public static method Make(): () -> K "
        "    var f := lambda() => new K(5); "
        "    return f; "
        "  end Make; "
        "  public static method Main(): int "
        "    return Program.Make()().N; "
        "  end Main; "
        "end Program;";
}

// Route 2 (issue p14): a method-group conversion of an `owned K` producer.
std::string methodGroupOfOwnedProducer(const char* retSlot) {
    return std::string("module M; ") + kKlass +
        "public class Program "
        "  public static method Fresh(): owned K => new K(5); "
        "  public static method Make(): " + retSlot + " "
        "    return Program.Fresh; "
        "  end Make; "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end Program;";
}

// Route 3: a bound variable `() -> owned K` retyped to `() -> K`.
std::string boundVariable(const char* target) {
    return std::string("module M; ") + kKlass +
        "public class Program "
        "  public static method Main(): int "
        "    var f: () -> owned K := lambda() => new K(5); "
        "    var g: " + target + " := f; "
        "    return 0; "
        "  end Main; "
        "end Program;";
}

}  // namespace

// Route 1: RED on r30-c1 (the returned closure `f : () -> owned K` is
// admitted at the `() -> K` return slot).
BEZEL_TEST(sema_fresh_owned_function_value_9105,
           returned_local_closure_into_plain_slot_rejected_9105) {
    auto r = parseSemaAll(returnedLocalClosure());
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "return slot is `owned`"));
}

// Route 2: RED on r30-c1 (`return Program.Fresh;` forms `() -> owned K` and
// retypes it to `() -> K`).
BEZEL_TEST(sema_fresh_owned_function_value_9105,
           method_group_of_owned_producer_into_plain_slot_rejected_9105) {
    auto r = parseSemaAll(methodGroupOfOwnedProducer("() -> K"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "return slot is `owned`"));
}

// Route 3: RED on r30-c1 (`var g: () -> K := f`, `f: () -> owned K`).
BEZEL_TEST(sema_fresh_owned_function_value_9105,
           bound_owned_variable_into_plain_slot_rejected_9105) {
    auto r = parseSemaAll(boundVariable("() -> K"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "return slot is `owned`"));
}

// CONTROL (green on both arms): the legal `owned` target spelling of
// route 2 — `Make(): () -> owned K { return Program.Fresh; }` — is
// admitted (§28.3.5 row 1: the marker transfers). RED under the wrong fix
// "reject every method-group of an `owned` producer at a function-typed
// slot" (a fix that keys on the SOURCE's marker alone).
BEZEL_TEST(sema_fresh_owned_function_value_9105,
           owned_target_method_group_still_admitted_9105) {
    auto r = parseSemaAll(methodGroupOfOwnedProducer("() -> owned K"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL (green on both arms): the `owned`-to-`owned` bound variable of
// route 3 is admitted. RED under the wrong fix "reject any function value
// whose return slot is `owned` at any function-typed slot" (a fix that does
// not read the TARGET's marker).
BEZEL_TEST(sema_fresh_owned_function_value_9105,
           owned_to_owned_bound_variable_still_admitted_9105) {
    auto r = parseSemaAll(boundVariable("() -> owned K"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
