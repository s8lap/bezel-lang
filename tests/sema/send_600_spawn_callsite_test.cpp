// #600 contract — §30.14 Domain.Spawn call-site rules.
//
// These tests translate the Spawn call-site constraints from §30.14 ("Spawning"
// sub-section) into sema diagnostics. They are EXPECTED TO FAIL against current
// main: sema has no Domain.Spawn or sendable-property checking.
// The implementation lane re-enables them from CMakeLists.txt.
//
// Spec derivation (§30.14 "Spawning"):
//   "The entry must be capture-free: a non-capturing lambda or a static method
//    reference. A capturing lambda or an instance method reference is rejected."
//   "Argument and result types must be sendable."
//   §30.14 / stdlib §6.8 — Domain.Spawn forms.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasNotSendableDiag(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("not sendable") != std::string::npos ||
            d.message.find("sendable") != std::string::npos) {
            return true;
        }
    }
    return false;
}

// True iff any diagnostic mentions "capture-free" or "capture" in a spawn
// context (the spec says capturing lambdas are rejected at the entry position).
bool hasCaptureDiag(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("capture") != std::string::npos ||
            d.message.find("entry") != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Accepted forms: static method reference / non-capturing lambda entry.
// ---------------------------------------------------------------------------

// §30.14: A static method reference as entry with no args → accepted.
// Domain.Spawn(MyClass.StaticEntry) must compile without error.
BEZEL_TEST(send_600_spawn_callsite,
           static_method_ref_entry_accepted_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "class Worker "
        "  public static method Run() end Run; "
        "end Worker; "
        "class Main "
        "  public static async method Go(): Task "
        "    using var t := Domain.Spawn(Worker.Run); "
        "    await t; "
        "  end Go; "
        "end Main;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §30.14: A non-capturing lambda entry → accepted.
BEZEL_TEST(send_600_spawn_callsite,
           non_capturing_lambda_entry_accepted_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "class Main "
        "  public static async method Go(): Task "
        "    using var t := Domain.Spawn(() => ()); "
        "    await t; "
        "  end Go; "
        "end Main;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §30.14: static method with a sendable value-type arg → accepted.
BEZEL_TEST(send_600_spawn_callsite,
           static_method_with_sendable_int_arg_accepted_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "class Worker "
        "  public static method Run(n: int) end Run; "
        "end Worker; "
        "class Main "
        "  public static async method Go(): Task "
        "    using var t := Domain.Spawn(Worker.Run, 42); "
        "    await t; "
        "  end Go; "
        "end Main;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// Rejected forms: capturing lambda / instance method reference.
// ---------------------------------------------------------------------------

// §30.14: A capturing lambda (captures local variable) → rejected.
// "A capturing lambda or an instance method reference is rejected — captures
// and receivers are borrows of the origin domain."
BEZEL_TEST(send_600_spawn_callsite,
           capturing_lambda_entry_rejected_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "class Main "
        "  public static async method Go(): Task "
        "    var x: int := 10; "
        "    using var t := Domain.Spawn(() => x := x + 1); "
        "    await t; "
        "  end Go; "
        "end Main;");
    // Rejected: lambda captures local variable 'x' → not capture-free.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §30.14: An instance method reference (receiver is a borrow of origin domain)
// → rejected.
BEZEL_TEST(send_600_spawn_callsite,
           instance_method_ref_entry_rejected_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "class Worker "
        "  public method Run() end Run; "
        "end Worker; "
        "class Main "
        "  public static async method Go(): Task "
        "    using var w := new Worker(); "
        "    using var t := Domain.Spawn(w.Run); "
        "    await t; "
        "  end Go; "
        "end Main;");
    // Rejected: w.Run is an instance method reference (receiver is a borrow).
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// Rejected: non-sendable argument type at spawn.
// ---------------------------------------------------------------------------

// §30.14: class-typed arg not declared `owned` in the entry signature →
// rejected. The arg must be owned at the call site.
BEZEL_TEST(send_600_spawn_callsite,
           class_arg_not_owned_in_entry_sig_rejected_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "sealed class Msg "
        "  public Val: int := 0; "
        "  destructor end destructor; "
        "end Msg; "
        // The entry takes Msg by borrow (not owned) — invalid for spawn.
        "class Worker "
        "  public static method Run(m: Msg?) end Run; "
        "end Worker; "
        "class Main "
        "  public static async method Go(): Task "
        "    using var msg := new Msg(); "
        "    using var t := Domain.Spawn(Worker.Run, msg); "
        "    await t; "
        "  end Go; "
        "end Main;");
    // Rejected: entry signature takes Msg by borrow, not owned transfer.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §30.14: non-sendable arg type (open class) → rejected naming "sendable".
BEZEL_TEST(send_600_spawn_callsite,
           non_sendable_class_arg_rejected_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "class OpenMsg "   // open class → not sendable
        "  public Val: int := 0; "
        "  destructor end destructor; "
        "end OpenMsg; "
        "class Worker "
        "  public static method Run(owned m: OpenMsg) end Run; "
        "end Worker; "
        "class Main "
        "  public static async method Go(): Task "
        "    using var msg := new OpenMsg(); "
        "    using var t := Domain.Spawn(Worker.Run, move msg); "
        "    await t; "
        "  end Go; "
        "end Main;");
    // Rejected: OpenMsg is not sendable (not sealed → not transfer-closed).
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotSendableDiag(r));
}

// §30.14: non-sendable result type in Spawn (open class result) → rejected.
BEZEL_TEST(send_600_spawn_callsite,
           non_sendable_result_type_rejected_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "class OpenResult "   // open class → not sendable
        "  public Val: int := 0; "
        "  destructor end destructor; "
        "end OpenResult; "
        "class Worker "
        "  public static method Produce(): owned OpenResult "
        "    return new OpenResult(); "
        "  end Produce; "
        "end Worker; "
        "class Main "
        "  public static async method Go(): Task "
        "    using var t := Domain.Spawn(Worker.Produce); "
        "  end Go; "
        "end Main;");
    // Rejected: OpenResult is not sendable → invalid as spawn result type.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotSendableDiag(r));
}
