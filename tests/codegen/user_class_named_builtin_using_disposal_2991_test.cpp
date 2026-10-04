// Regression tests for issue #2991 (language spec §11.3 user-declared class
// names / §28.3 scope-bound auto-drop / §28.9 runtime-managed types, v0.240):
// a USER class whose name happens to collide with a built-in's, bound to a
// `using var`, was released with the BUILT-IN's C release.
//
//   class EventSource public readonly property Event: int := 5; end EventSource;
//   using var src := new EventSource();     // -> bz_event_source_free(userobj)
//                                           //    SIGSEGV, no diagnostic
//
// §11.3 reserves no names. The program above contains no built-in at all — it
// is an ordinary user class with a real `ast::TypeDecl` — so this is memory
// unsafety reachable from a legal program with nothing exotic in it, and with
// nothing said at compile time. (#2935 already pins the mirror-image rule for
// the DIAGNOSTIC predicate — `user_type_sharing_a_builtin_name_is_not_caught_
// spec_11_3` — and nothing pinned it for the DISPOSAL predicates.)
//
// ── THE SCOPE THE ISSUE STATED, AND WHAT MEASUREMENT FOUND ──────────────────
// #2991 named ~6 sibling arms and reported only `EventSource` crashing, with a
// user `Stopwatch` measured "fine, exit=5". Both halves of that were wrong.
//
// The chain probes NINETEEN names, and the "fine" verdict came from a WEAK
// oracle: a user class with no owned state, freed with a release that happens
// to be a plain `free()`, exits 5 and looks correct. Give the user class ONE
// owned field — so that "the user's destructor ran" is observable — and the
// silent half of the family appears. MEASURED on 5feaae2, one user class per
// name carrying `private owned _p: Payload`, in a 200-iteration `using var`
// loop under generated-code ASan/LSan:
//
//   SEGV / abort / hang (10)   BinarySerializer, JsonSerializer, FileStream,
//                              CancellationTokenSource, Channel (HANG — no
//                              exit code at all, only a timeout),
//                              TaskCompletionSource, OwningTaskCompletionSource,
//                              EventSource, AsyncEventSource, StringBuilder
//   SILENT LEAK (8)            BinaryDeserializer, JsonDeserializer,
//                              Stopwatch (3184 B / 199 allocs — the issue's
//                              own "fine" control), Mutex, Semaphore, Shared,
//                              Weak, Buffer (7960 B / 398 allocs for the four
//                              that also leak the wrapper)
//   ALREADY CLEAN (2)          Random, Atomic — the two names #2973 had ALREADY
//                              routed through a resolved-type guard. That is
//                              the mechanical proof that the guard shape below
//                              is the fix, not a hypothesis about one.
//   NOT REACHABLE (1)          TaskScope — sema rejects a plain `using var` of
//                              *any* type named TaskScope before codegen sees
//                              it (a separate, LOUD name-collision defect;
//                              filed separately).
//
// So 18 of 19, not 1 of 6 — and the half that fails SILENTLY is the half the
// issue reported as healthy.
//
// ── THE FIX: ONE FACT, ASKED ONCE ───────────────────────────────────────────
// Each arm probed three ways. Two probes went through a lambda that already
// required a declaration-less built-in; the third read `ne->type->name->
// parts[0]` BARE off the AST. `Emitter::newExprBuiltinHead` (LlvmBackend_Setup
// .cpp) now answers that third probe for every arm from ONE place, by asking
// `builtinTypeName` — the fact the compiler already computes, which reads both
// TypeRef shapes a built-in arrives in and yields nothing for a user
// declaration. Asking it once is the point: an arm added later inherits the
// guard rather than copying its neighbour's defect (the shape #2921/#2967/#2973
// chose for this same chain).
//
// A leak is not the worst outcome; a wild free is. `newExprBuiltinHead` returns
// nothing when the type cannot be resolved, so the failure direction is the
// recoverable one.

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <iostream>
#include <string>

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A user class of the given name carrying BOTH observables that matter:
//   * `private owned _p: Payload` — the LEAK oracle. Only the user class's own
//     disposal cascades into an owned field, so if a built-in's C release runs
//     instead, the 200 Payloads leak (this is what exposed `Stopwatch`, which
//     the issue measured as fine).
//   * `public readonly property Tag` — the VALUE oracle, so "exit 0" cannot be
//     reached by a program that silently did nothing.
// 200 iterations because a single `Main` is not a reliable oracle for a
// release path: a per-iteration miss shows up as 200 allocations, which no
// stray single allocation elsewhere can be mistaken for.
std::string userClassNamed(const char* name) {
    std::string n = name;
    return
        "module M; "
        "class Payload public readonly property N: int := 7; end Payload; "
        "class " + n + " "
        "  private owned _p: Payload := new Payload(); "
        "  public readonly property Tag: int := 3; "
        "end " + n + "; "
        "class C public static method Main(): int "
        "  var i := 0; var acc := 0; "
        "  while i < 200 do "
        "    using var s := new " + n + "(); "
        "    acc := acc + s.Tag; "
        "    i := i + 1; "
        "  end; "
        "  if acc <> 600 then return 9; end; "
        "  return 0; "
        "end Main; end C;";
}

// Runs one name and reports WHICH name failed — a grouped test that only says
// "false" would send the next reader back to re-derive the grid.
void expectUserClassDisposedCorrectly(const char* name) {
    auto r = runAotChecked(userClassNamed(name));
    if (!r.ran || r.exitCode != 0) {
        std::cout << "    #2991: user class named `" << name
                  << "` was not disposed as a user class"
                  << " (ran=" << (r.ran ? "true" : "false")
                  << ", exit=" << r.exitCode << ")\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
}

}  // namespace

// ── The filed repro, verbatim ───────────────────────────────────────────────

BEZEL_TEST(user_class_named_builtin_using_disposal_2991,
           user_event_source_using_var_does_not_take_the_builtin_release_spec_11_3) {
    // #2991's program exactly. On 5feaae2: `Segmentation fault`, exit 139, and
    // no diagnostic — `bz_event_source_free` walking a user object's fields as
    // a subscriber list. `"5".Length` is 1.
    auto r = runAot(
        "module M; "
        "class EventSource public readonly property Event: int := 5; end EventSource; "
        "class C public static method Main(): int "
        "  using var src := new EventSource(); "
        "  return src.Event.ToString().Length; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(1, r.exitCode);
}

// ── The control the issue reported as healthy, refuted ──────────────────────

BEZEL_TEST(user_class_named_builtin_using_disposal_2991,
           user_stopwatch_runs_its_own_disposal_not_bz_stopwatch_free_spec_28_9) {
    // #2991: "A user class named `Stopwatch` in the same shape — exit=5,
    // correct. So the family is not uniformly live." It IS live: `Stopwatch`
    // took `bz_stopwatch_free` (a plain free of the object) and therefore never
    // ran the user class's disposal, so its owned field leaked — 3184 bytes in
    // 199 allocations on 5feaae2. The exit code was right because a wrong free
    // of the right pointer still reclaims the object; only the CASCADE was lost.
    expectUserClassDisposedCorrectly("Stopwatch");
}

// ── The wild-free half of the family ────────────────────────────────────────

BEZEL_TEST(user_class_named_builtin_using_disposal_2991,
           wild_free_names_are_disposed_as_user_classes_spec_28_3) {
    // Each of these crashed on 5feaae2, in the built-in release named by its
    // arm: BinarySerializer/JsonSerializer/EventSource/AsyncEventSource/
    // StringBuilder inside the allocator (an invalid free), FileStream inside
    // `_IO_fflush`, CancellationTokenSource inside `bz_cts_dispose`, and the
    // two TaskCompletionSource names inside `__pthread_mutex_lock_full` —
    // i.e. a user object's bytes being read as a FILE*, a token list, and a
    // pthread mutex respectively.
    for (const char* n : {"BinarySerializer", "JsonSerializer", "FileStream",
                          "CancellationTokenSource", "TaskCompletionSource",
                          "OwningTaskCompletionSource", "EventSource",
                          "AsyncEventSource", "StringBuilder"})
        expectUserClassDisposedCorrectly(n);
}

BEZEL_TEST(user_class_named_builtin_using_disposal_2991,
           user_channel_terminates_rather_than_hanging_spec_28_3) {
    // Kept apart because its failure mode is the one no exit code and no
    // sanitizer reports: on 5feaae2 this HUNG (`bz_channel_dispose` faulting
    // pending receivers on a user object), and the only evidence was the 30s
    // harness timeout. A test that only asserted `exitCode == 0` on a grouped
    // run would have burned the whole shard budget here.
    expectUserClassDisposedCorrectly("Channel");
}

// ── The SILENT half — the reason `exit 0` is not an oracle ──────────────────

BEZEL_TEST(user_class_named_builtin_using_disposal_2991,
           silent_leak_names_cascade_into_the_owned_field_spec_28_9) {
    // None of these crashed on 5feaae2 and all of them returned the right
    // value; every one leaked its owned field because the user destructor never
    // ran (3200 B / 200 allocs for the two deserializers, 3184 B / 199 for
    // Buffer, 7960 B / 398 for Mutex/Semaphore/Shared/Weak). §28.9: the owner's
    // release point is where the undischarged value is freed — for a user class
    // that release point is its own disposal, cascade included.
    for (const char* n : {"BinaryDeserializer", "JsonDeserializer", "Mutex",
                          "Semaphore", "Shared", "Weak", "Buffer"})
        expectUserClassDisposedCorrectly(n);
}

// ── The already-clean control: what the fix was derived FROM ────────────────

BEZEL_TEST(user_class_named_builtin_using_disposal_2991,
           the_two_names_2973_already_guarded_stay_clean_spec_28_3) {
    // `Random` and `Atomic` were the only two names in the grid that were
    // already correct on 5feaae2, because #2973 had put a resolved-type guard
    // on THAT arm's probe and only that one. Pinned so the shared helper cannot
    // regress the case that proved it works.
    for (const char* n : {"Random", "Atomic"})
        expectUserClassDisposedCorrectly(n);
}

// ── The other direction: no GENUINE built-in may stop being released ────────
// `newExprBuiltinHead` is a NARROWING of the previous bare-name comparison, so
// the risk it carries is the opposite one — a real built-in that stops matching
// its arm and leaks. Measured clean before AND after across every constructible
// built-in the chain claims; these pin the representative shapes, including the
// annotated spelling (which takes a different probe) so a future "simplification"
// that deletes the NewExpr probe entirely is caught here.

BEZEL_TEST(user_class_named_builtin_using_disposal_2991,
           genuine_builtins_are_still_released_at_scope_exit_spec_28_3) {
    auto loop = [](const char* decl) {
        return std::string(
            "module M; import System.Diagnostics; class C public static method Main(): int "
            "  var i := 0; while i < 200 do ") + decl + " i := i + 1; end; "
            "  return 0; end Main; end C;";
    };
    for (const char* d : {"using var s := new EventSource<int>();",
                          "using var s := new AsyncEventSource<int>();",
                          "using var s := new StringBuilder();",
                          "using var s := new Channel<int>();",
                          "using var s := new Stopwatch();",
                          "using var s := new Mutex();",
                          "using var s := new Semaphore(2);",
                          "using var s := new __Buffer<int>(4);",
                          "using var s := new CancellationTokenSource();",
                          "using var s := new TaskCompletionSource<int>();",
                          // the ANNOTATED spelling — a different probe
                          "using var s: Channel<int> := new Channel<int>();",
                          "using var s: StringBuilder := new StringBuilder();",
                          "using var s: Mutex := new Mutex();"}) {
        auto r = runAotChecked(loop(d));
        if (!r.ran || r.exitCode != 0)
            std::cout << "    #2991 control regressed: " << d
                      << " (ran=" << (r.ran ? "true" : "false")
                      << ", exit=" << r.exitCode << ")\n";
        BEZEL_EXPECT_TRUE(r.ran);
        BEZEL_EXPECT_EQ(0, r.exitCode);
    }
}
