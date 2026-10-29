// Regression test for issue #7763 -- an extension block declared on the BARE
// class `Task` was invisible to a `Task<T>` / `OwningTask<T>` receiver, so
// `t.Done` over `extension Task { property Done => this.IsCompleted }` was
// dropped in codegen (internal #2434 warning, exit 0) and, since #7731's
// closed-surface check, rejected with the misleading "`Task` has no member
// `Done`".
//
// spec §30.3 (v0.350): `public class Task<T>: Task` and
// `public class OwningTask<T>: Task where T: class` -- `Task` is the BASE CLASS
// of both. spec §26.2 (v0.350): "What can be extended: Classes (including
// `Object`)." spec §26.5 rule 2 (v0.350): the compiler searches "for extension
// blocks on `x`'s type, base types, or implemented interfaces". So an
// `extension Task` applies to a `Task<T>` and an `OwningTask<T>` receiver, and
// an `extension Object` applies to every one of them. §26.5.1: the more
// specific block (a `Task<T>` head, for a `Task<T>` receiver) wins.
// `extension Task<T>` is NOT a block on a base of `OwningTask<T>` (whose base
// is the bare `Task`), so it does not apply there.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

const char* kHelpers =
    "class C "
    "  public static async method V(): Task "
    "    await Task.Delay(1); "
    "  end V; "
    "  public static async method P(): OwningTask<Object> "
    "    return new Object(); "
    "  end P; ";

}  // namespace

// The issue's repro: a property on the bare `Task`, read off a `Task<int>`.
BEZEL_TEST(task_extension_base_head_7763, property_on_bare_task_read_off_task_t) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; import System; "
            "extension Task "
            "  public property Done: bool => this.IsCompleted; "
            "end extension; "
            "class C "
            "  public static method Main(): int "
            "    using var t := Task.FromResult(1); "
            "    Console.WriteLine(t.Done.ToString()); "
            "    return 0; "
            "  end Main; "
            "end C;"),
        std::string("True"));
}

// Property and method, on all three receiver kinds (`Task`, `Task<int>`,
// `OwningTask<Object>`).
BEZEL_TEST(task_extension_base_head_7763, property_and_method_on_all_three_receivers) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            std::string("module M; import System; "
                        "extension Task "
                        "  public property Done: bool => this.IsCompleted; "
                        "  public method Tag(k: int): int => k + 1; "
                        "end extension; ") +
            kHelpers +
            "  public static method Main(): int "
            "    using var a := Task.FromResult(1); "
            "    using var b := V(); "
            "    using var c := P(); "
            "    b.Wait(); c.Wait(); "
            "    Console.WriteLine(a.Done.ToString() + b.Done.ToString() + c.Done.ToString()); "
            "    Console.WriteLine(a.Tag(1).ToString() + b.Tag(2).ToString() + c.Tag(3).ToString()); "
            "    return 0; "
            "  end Main; "
            "end C;"),
        std::string("TrueTrueTrue\n234"));
}

// §26.5.1: the `Task<T>` block is more specific than the `Task` block for a
// `Task<int>` receiver; the `Task` receiver reaches only the `Task` block.
BEZEL_TEST(task_extension_base_head_7763, more_specific_task_t_block_wins) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            std::string("module M; import System; "
                        "extension Task "
                        "  public property Which: string => \"base\"; "
                        "end extension; "
                        "extension Task<T> "
                        "  public property Which: string => \"generic\"; "
                        "end extension; ") +
            kHelpers +
            "  public static method Main(): int "
            "    using var a := Task.FromResult(1); "
            "    using var b := V(); "
            "    Console.WriteLine(a.Which + b.Which); "
            "    return 0; "
            "  end Main; "
            "end C;"),
        std::string("genericbase"));
}

// §26.2 / §26.5 rule 2: `Object` is a base of a task like of any class.
BEZEL_TEST(task_extension_base_head_7763, object_extension_property_read_off_task) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; import System; "
            "extension Object "
            "  public property Tagged: string => \"obj\"; "
            "end extension; "
            "class C "
            "  public static method Main(): int "
            "    using var a := Task.FromResult(1); "
            "    Console.WriteLine(a.Tagged); "
            "    return 0; "
            "  end Main; "
            "end C;"),
        std::string("obj"));
}

// Through the null-conditional access (`?.`, §7.5) on a nullable task.
BEZEL_TEST(task_extension_base_head_7763, bare_task_extension_through_null_conditional) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; import System; "
            "extension Task "
            "  public property Done: bool => this.IsCompleted; "
            "end extension; "
            "class C "
            "  public static method Main(): int "
            "    using var t0 := Task.FromResult(2); "
            "    var t: Task<int>? := t0; "
            "    var d := t?.Done; "
            "    if d <> null then Console.WriteLine(d!.ToString()); end; "
            "    var n: Task<int>? := null; "
            "    var e := n?.Done; "
            "    Console.WriteLine((e is null).ToString()); "
            "    return 0; "
            "  end Main; "
            "end C;"),
        std::string("True\nTrue"));
}

// Control: `extension Task<T>` is not a block on a base of `OwningTask<T>`
// (whose base is the bare `Task`), so the read is still a compile error.
BEZEL_TEST(task_extension_base_head_7763, task_t_block_does_not_apply_to_owning_task) {
    auto r = runAot(
        std::string("module M; import System; "
                    "extension Task<T> "
                    "  public property Done: bool => this.IsCompleted; "
                    "end extension; ") +
        kHelpers +
        "  public static method Main(): int "
        "    using var c := P(); "
        "    c.Wait(); "
        "    Console.WriteLine(c.Done.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("`OwningTask` has no member `Done`") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

// §26.5.1: an `extension Object` member never outranks a same-named member of
// a more specific block (`Task<T>`, or the bare `Task`, both subtypes of
// `Object`), on a `Task<int>` receiver.
BEZEL_TEST(task_extension_base_head_7763, object_block_loses_to_task_t_block) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; import System; "
            "extension Object "
            "  public property Hello: string => \"obj\"; "
            "end extension; "
            "extension Task<T> "
            "  public property Hello: string => \"taskT\"; "
            "end extension; "
            "class C "
            "  public static method Main(): int "
            "    using var a := Task.FromResult(1); "
            "    Console.WriteLine(a.Hello); "
            "    return 0; "
            "  end Main; "
            "end C;"),
        std::string("taskT"));
}

BEZEL_TEST(task_extension_base_head_7763, object_block_loses_to_bare_task_block) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            std::string("module M; import System; "
                        "extension Object "
                        "  public property Hello: string => \"obj\"; "
                        "end extension; "
                        "extension Task "
                        "  public property Hello: string => \"task\"; "
                        "end extension; ") +
            kHelpers +
            "  public static method Main(): int "
            "    using var a := Task.FromResult(1); "
            "    using var c := P(); "
            "    c.Wait(); "
            "    Console.WriteLine(a.Hello + c.Hello); "
            "    return 0; "
            "  end Main; "
            "end C;"),
        std::string("tasktask"));
}

// §26.5 rule 1: a member `Task` DECLARES (IsCompleted, spec §30.3) commits
// instance resolution by name, so an `extension Object` member of that name is
// never consulted: the read is the task's own (a completed task: True).
BEZEL_TEST(task_extension_base_head_7763, object_extension_never_shadows_declared_task_member) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; import System; "
            "extension Object "
            "  public property IsCompleted: bool => false; "
            "end extension; "
            "class C "
            "  public static method Main(): int "
            "    using var a := Task.FromResult(1); "
            "    Console.WriteLine(a.IsCompleted.ToString()); "
            "    return 0; "
            "  end Main; "
            "end C;"),
        std::string("True"));
}

// Same rule for a METHOD name: `Wait` is declared by `Task` (arity 0), so a
// `Wait(k: int)` extension is not a fallback for `a.Wait(3)`; the extension's
// result (3) must never be produced, and the arity-mismatched call is rejected
// (issue #7869, spec §30.3).
BEZEL_TEST(task_extension_base_head_7763, object_extension_method_never_shadows_declared_wait) {
    auto r = runAot(
        "module M; import System; "
        "extension Object "
        "  public method Wait(k: int): int => k; "
        "end extension; "
        "class C "
        "  public static method Main(): int "
        "    using var a := Task.FromResult(1); "
        "    Console.WriteLine(a.Wait(3).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.stdoutText.find("3") == std::string::npos);
    bool rejected = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("`Wait` of `Task` takes no arguments") !=
                std::string::npos)
            rejected = true;
    BEZEL_EXPECT_TRUE(rejected);
}
