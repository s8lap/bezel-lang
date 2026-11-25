// Sema — issue #9127 (spec §27.1.1 fourth bullet, v0.359: "Modifier slots
// (`owned`/`ref`/`out`/`out owned`/`scoped`) must match exactly"; §28.3.5 rows
// 1 and 4, v0.359): a function value whose RETURN slot is the plain concrete
// class/interface `K` — a variable typed `() -> K`, or a method-group
// reference to a borrow-returning method — may not be retyped to a function
// type whose return slot is `owned` (`() -> owned K`).
//
// WHY. Row 4's function-value-invocation cell: a call through a plain `() -> K`
// slot is a "Borrow ... The result is never owned". Row 1: a call through an
// `owned` slot is "Owned", so the CALLER disposes every result. A plain value
// retyped to the `owned` slot hands the caller's disposal obligation a borrow
// of storage someone else owns: a heap-use-after-free (#9127's two repros
// under ASan). This is the reverse of #9105 (an `owned` value at a plain slot,
// f970d2dd1).
//
// ROWS. One RED row per fixed route (each RED on the base: the program is
// admitted and `hasErrors()` is false) and two boundary controls, each GREEN on
// both arms and RED under a named wrong fix.
//
// Spec-unclear: none for these routes. Not touched (`narrow`): the
// mint-announcing return slots (`Task`-family, iterable heads) and a bare
// type-parameter slot, whose substitution-dependent announcement rows (§28.3.5
// rows 2/3/5) are a separate question.

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

// Route 1 (issue repro 1): a bound `() -> K` variable retyped to `() -> owned K`.
std::string boundPlainVariable(const char* rhs) {
    return std::string("module M; ") + kKlass +
        "public class Program "
        "  public static method Main(): int "
        "    using var k := new K(7); "
        "    var f: () -> K := lambda() => k; "
        "    var g: () -> owned K := " + rhs + "; "
        "    using var z := g(); "
        "    return z.N; "
        "  end Main; "
        "end Program;";
}

// Route 2 (issue repro 2): a method group of a borrow-returning method.
std::string methodGroupOfBorrowingMethod() {
    return std::string("module M; ") + kKlass +
        "public class Holder "
        "  private owned k: K; "
        "  public constructor(n: int) k := new K(n); end constructor; "
        "  public method Lend(): K => k; "
        "end Holder; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(7); "
        "    var g: () -> owned K := h.Lend; "
        "    using var z := g(); "
        "    return z.N; "
        "  end Main; "
        "end Program;";
}

// Route 3: the plain value reaches the owned slot as a call argument.
std::string plainValueAsArgument() {
    return std::string("module M; ") + kKlass +
        "public class Program "
        "  public static method Take(f: () -> owned K): int "
        "    using var z := f(); "
        "    return z.N; "
        "  end Take; "
        "  public static method Main(): int "
        "    using var k := new K(7); "
        "    var f: () -> K := lambda() => k; "
        "    return Program.Take(f); "
        "  end Main; "
        "end Program;";
}

// Route 4: the plain value is returned at an `owned` function-typed slot.
std::string plainValueReturned() {
    return std::string("module M; ") + kKlass +
        "public class Program "
        "  public static method Wrap(f: () -> K): () -> owned K "
        "    return f; "
        "  end Wrap; "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end Program;";
}

}  // namespace

BEZEL_TEST(sema_plain_function_value_into_owned_slot_9127,
           bound_plain_variable_into_owned_slot_rejected_9127) {
    auto r = parseSemaAll(boundPlainVariable("f"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "but the target's is `owned`"));
}

BEZEL_TEST(sema_plain_function_value_into_owned_slot_9127,
           method_group_of_borrowing_method_into_owned_slot_rejected_9127) {
    auto r = parseSemaAll(methodGroupOfBorrowingMethod());
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "but the target's is `owned`"));
}

BEZEL_TEST(sema_plain_function_value_into_owned_slot_9127,
           plain_value_as_call_argument_to_owned_slot_rejected_9127) {
    auto r = parseSemaAll(plainValueAsArgument());
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "but the target's is `owned`"));
}

BEZEL_TEST(sema_plain_function_value_into_owned_slot_9127,
           plain_value_returned_at_owned_slot_rejected_9127) {
    auto r = parseSemaAll(plainValueReturned());
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "but the target's is `owned`"));
}

// CONTROL (green on both arms): a lambda LITERAL with a fresh body at an
// `owned` slot is legal (§28.3.5 row 1 callee-side admission: "every return
// path supplies a fresh or owned value"). RED under the wrong fix "reject
// every lambda whose own inferred type is plain at an `owned` slot" (a fix
// that does not skip the fresh lambda literal).
BEZEL_TEST(sema_plain_function_value_into_owned_slot_9127,
           fresh_lambda_literal_at_owned_slot_still_admitted_9127) {
    auto r = parseSemaAll(std::string("module M; ") + kKlass +
        "public class Program "
        "  public static method Main(): int "
        "    var g: () -> owned K := lambda() => new K(5); "
        "    using var z := g(); "
        "    return z.N; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL (green on both arms): an `owned` function value (a method group of
// an `owned K` producer) at an `owned` slot is admitted. RED under the wrong
// fix "reject any function value at an owned slot unless it is a lambda".
BEZEL_TEST(sema_plain_function_value_into_owned_slot_9127,
           owned_method_group_at_owned_slot_still_admitted_9127) {
    auto r = parseSemaAll(std::string("module M; ") + kKlass +
        "public class Program "
        "  public static method Fresh(): owned K => new K(5); "
        "  public static method Main(): int "
        "    var g: () -> owned K := Program.Fresh; "
        "    using var z := g(); "
        "    return z.N; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
