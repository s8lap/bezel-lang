// Tests for the v0.168 owning-rename revision (#1372).
//
// The owning *family* of stdlib types was renamed to spell the element axis
// with the prefix `Owning` (the container axis keeps the `owned` qualifier):
//
//   OwnedArray/Buffer/Dictionary/Set/Stack/Queue/LinkedList → Owning*
//   OwnedTask → OwningTask,  OwnedTaskCompletionSource → OwningTaskCompletionSource
//   the owning INTERFACES additionally gain the standard `I` prefix:
//     OwnedEnumerable/Enumerator            → IOwningEnumerable/Enumerator
//     OwnedAsyncEnumerable/AsyncEnumerator  → IOwningAsyncEnumerable/AsyncEnumerator
//     IOwnedList/IOwnedCollection           → IOwningList/IOwningCollection
//   GetOwnedEnumerator()  → GetOwningEnumerator()  (method; exercised by the
//                            codegen / conformance corpus, not here)
//
// See rationale §3.9 and the v0.168 changelog. The compiler's type registry
// (builtinTypeArity, TypeResolution) now recognises the NEW names; the OLD
// names no longer resolve and the unresolved-type diagnostic special-cases them
// with a rename hint (`… was renamed … in v0.168 (#1372)`).
//
// This file pins both directions: (1) each renamed family member resolves under
// its new name; (2) a spot-check of the old names fails WITH the hint text.

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

// Run the full sema pipeline on the given source.
bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff any diagnostic message contains the given substring.
bool anyDiagContains(const bezel::test::SemaResult& r, std::string_view substr) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find(std::string(substr)) != std::string::npos)
            return true;
    }
    return false;
}

// Resolve `typeStr` as a method parameter type and report whether the
// type-resolution pass emitted the "unresolved type name" error for it.
bool typeIsUnresolved(const std::string& typeStr) {
    std::string src = "module M; class C "
                      "public method M(p: " + typeStr + "): int "
                      "return 0; end M; end C;";
    auto r = parseSemaAll(src);
    return anyDiagContains(r, "unresolved type name");
}

} // namespace

// ===========================================================================
// (1) The NEW names resolve — one representative per renamed family member.
// ===========================================================================

// §4.5 / §4.14 / §4.7 (stdlib): the renamed owning CONTAINER types resolve.
BEZEL_TEST(sema_owning_rename_1372, new_container_names_resolve_spec_4) {
    for (const char* t : {"OwningArray<int>", "OwningBuffer<int>",
                          "OwningDictionary<int, int>", "OwningSet<int>",
                          "OwningStack<int>", "OwningQueue<int>",
                          "OwningLinkedList<int>"}) {
        BEZEL_EXPECT_FALSE(typeIsUnresolved(t));
    }
}

// §6.6 (stdlib): the renamed owning TASK types resolve.
BEZEL_TEST(sema_owning_rename_1372, new_task_names_resolve_spec_6_6) {
    for (const char* t : {"OwningTask<int>", "OwningTaskCompletionSource<int>"}) {
        BEZEL_EXPECT_FALSE(typeIsUnresolved(t));
    }
}

// §4.1–§4.3 / §29 (stdlib): the renamed owning INTERFACES (now `I`-prefixed)
// resolve.
BEZEL_TEST(sema_owning_rename_1372, new_interface_names_resolve_spec_4_29) {
    for (const char* t : {"IOwningList<int>", "IOwningCollection<int>",
                          "IOwningEnumerable<int>", "IOwningEnumerator<int>",
                          "IOwningAsyncEnumerable<int>",
                          "IOwningAsyncEnumerator<int>"}) {
        BEZEL_EXPECT_FALSE(typeIsUnresolved(t));
    }
}

// ===========================================================================
// (2) The OLD names no longer resolve, and the diagnostic carries the rename
//     hint. Spot-check across the distinct hint shapes (plain container, task,
//     an already-`I`-prefixed interface, and an interface that GAINS the `I`).
// ===========================================================================

// Old container name → hint pointing at the plain `Owning` rename.
BEZEL_TEST(sema_owning_rename_1372, old_owned_array_reports_hint_1372) {
    auto r = parseSemaAll("module M; class C "
                          "public method M(p: OwnedArray<int>): int "
                          "return 0; end M; end C;");
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "unresolved type name `OwnedArray`"));
    BEZEL_EXPECT_TRUE(anyDiagContains(
        r, "`OwnedArray` was renamed `OwningArray` in v0.168 (#1372)"));
}

// Old container name (stack) → hint.
BEZEL_TEST(sema_owning_rename_1372, old_owned_stack_reports_hint_1372) {
    auto r = parseSemaAll("module M; class C "
                          "public method M(p: OwnedStack<int>): int "
                          "return 0; end M; end C;");
    BEZEL_EXPECT_TRUE(anyDiagContains(
        r, "`OwnedStack` was renamed `OwningStack` in v0.168 (#1372)"));
}

// Old task name → hint.
BEZEL_TEST(sema_owning_rename_1372, old_owned_task_reports_hint_1372) {
    auto r = parseSemaAll("module M; class C "
                          "public method M(p: OwnedTask<int>): int "
                          "return 0; end M; end C;");
    BEZEL_EXPECT_TRUE(anyDiagContains(
        r, "`OwnedTask` was renamed `OwningTask` in v0.168 (#1372)"));
}

// Old already-`I`-prefixed interface → hint (I → I, name body renamed).
BEZEL_TEST(sema_owning_rename_1372, old_iowned_list_reports_hint_1372) {
    auto r = parseSemaAll("module M; class C "
                          "public method M(p: IOwnedList<int>): int "
                          "return 0; end M; end C;");
    BEZEL_EXPECT_TRUE(anyDiagContains(
        r, "`IOwnedList` was renamed `IOwningList` in v0.168 (#1372)"));
}

// Old UNprefixed interface → hint that ADDS the `I` prefix (the #1359 fold-in).
BEZEL_TEST(sema_owning_rename_1372, old_owned_enumerable_reports_hint_1372) {
    auto r = parseSemaAll("module M; class C "
                          "public method M(p: OwnedEnumerable<int>): int "
                          "return 0; end M; end C;");
    BEZEL_EXPECT_TRUE(anyDiagContains(
        r, "`OwnedEnumerable` was renamed `IOwningEnumerable` in v0.168 (#1372)"));
}
