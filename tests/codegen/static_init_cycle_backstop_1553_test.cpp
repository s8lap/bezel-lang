// fix #1553 / §22.13 (v0.181) rule 2 / §22.7 (v0.181): the static-initialization
// cycle RUNTIME BACKSTOP. The compile-time approximation (§22.13 rule 1,
// StaticInitCycles.cpp) rejects every cycle it can see through statically-
// resolvable calls, but a cycle routed through a FUNCTION VALUE (or virtual
// dispatch, or a module boundary) compiles. At runtime, when the initializer of
// a type re-enters ITSELF on the same thread, the runtime must synthesize a
// TypeInitializationException — message naming the re-entrant type, InnerException
// null (there is no original exception) — that escapes the interrupted
// initializer. Partially-initialized statics are NEVER observed.
//
// Before this fix:
//   * the int-cycle silently observed a DEFAULT 0 (the re-entrant read returned
//     the not-yet-initialized static's zero value) and computed a wrong result;
//   * the non-nullable-string cycle SEGFAULTED (the re-entrant read observed a
//     null non-nullable string and later dereferenced it).
//
// These are codegen tests (static-init timing + exception routing), so runJit is
// the right harness — the exit code captures the caught-exception outcome, and a
// segfault surfaces as `ran == false` (→ the -999 sentinel here) rather than a
// spurious value.
//
// Posture note (§22.13 rule 2): the backstop is SAME-THREAD-ONLY by design. A
// cycle spanning two concurrently-initializing domains would instead deadlock in
// §22.12 rule-1 blocking (that cross-domain synchronization is a separate,
// unimplemented concern, #1391); no cross-domain waits-for detector is promised
// or added here. The owner-thread global recorded per type is exactly what keeps
// the synthesized raise strictly same-thread.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// A static-init cycle A -> B -> A whose B->A edge is routed through a FUNCTION
// VALUE (`() -> int` field holding a lambda that reads A.Y), so the §22.13 rule-1
// compile-time approximation does NOT see it. A.Y has a NON-constant initializer,
// so its backing global is a default 0 while A.X is being computed — the shape
// that silently observed partial state before the fix.
const char* kIntCycle =
    "class A "
    "  public static readonly X: int := B.GetX(); "
    "  public static readonly Y: int := Seed(); "
    "  private static method Seed(): int => 1; "
    "end A; "
    "class B "
    "  private static _f: () -> int := () => A.Y; "
    "  public static method GetX(): int => _f() + 100; "
    "end B; ";

// The same cycle over a NON-NULLABLE string static. The re-entrant read observes
// A.Y still null (its initializer has not run) — a dereference of that null
// segfaulted before the fix.
const char* kStrCycle =
    "class A "
    "  public static readonly X: string := B.GetX(); "
    "  public static readonly Y: string := Make(); "
    "  private static method Make(): string => \"world\"; "
    "end A; "
    "class B "
    "  private static _f: () -> string := () => A.Y; "
    "  public static method GetX(): string => _f(); "
    "end B; ";
}  // namespace

// §22.13 rule 2: the int cycle raises a TypeInitializationException at the
// triggering access instead of silently returning 100 (the partial-state result).
// The catch confirms the exception is raised AND that its InnerException is null
// (the cycle-backstop signature, §22.13/§31.4). Before the fix Main returned 100.
BEZEL_TEST(static_init_cycle_backstop_1553,
           same_thread_reentrant_touch_raises_TIE_not_partial_state_spec_22_13) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kIntCycle +
        "class C public static method Main(): int "
        "  try "
        "    var v := A.X; "
        "    return v; "                 // pre-fix: 100 (partial state observed)
        "  catch var ex: TypeInitializationException => do "
        "    if ex.InnerException = null then return 55; else return 1; end; "
        "  end end; "
        "end Main; end C;"), 55);
}

// §22.13 rule 2: the non-nullable-string cycle raises a TypeInitializationException
// rather than segfaulting on a null non-nullable static. Before the fix this
// exited 139 (SIGSEGV → runJit ran == false → -999 sentinel); after, it is caught.
BEZEL_TEST(static_init_cycle_backstop_1553,
           nonnullable_string_cycle_raises_TIE_no_segfault_spec_22_13) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kStrCycle +
        "class C public static method Main(): int "
        "  try "
        "    var v := A.X; "
        "    return v.Length; "          // pre-fix: SIGSEGV (null deref)
        "  catch var ex: TypeInitializationException => do "
        "    if ex.InnerException = null then return 55; else return 1; end; "
        "  end end; "
        "end Main; end C;"), 55);
}

// §22.7 (v0.181): once the escaping synthesized TIE fails the type, it is
// permanently failed — every SUBSEQUENT touch re-raises a TypeInitializationException
// (§22.7 failed-state machinery), so both accesses catch. Pins that the backstop
// feeds the ordinary failed-state cache, not a one-shot raise.
BEZEL_TEST(static_init_cycle_backstop_1553,
           cached_failure_retouch_reraises_TIE_spec_22_7) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kIntCycle +
        "class C public static method Main(): int "
        "  var first := 0; "
        "  try var a := A.X; catch var e: TypeInitializationException => first := 1; end; "
        "  var second := 0; "
        "  try var b := A.X; catch var e: TypeInitializationException => second := 1; end; "
        "  return first * 10 + second; "  // 11 — both touches raise
        "end Main; end C;"), 11);
}

// §22.13 rule 2 / §22.7 (v0.181): the synthesized backstop instance is cached
// DIRECTLY (no double-wrap) — its InnerException is null even after it has escaped
// the interrupted initializer and been routed through the §22.7 wrapping path. A
// double-wrap would make InnerException a nested TypeInitializationException
// (non-null), which is what this pins does NOT happen.
BEZEL_TEST(static_init_cycle_backstop_1553,
           backstop_TIE_cached_directly_inner_null_not_double_wrapped_spec_22_7) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kStrCycle +
        "class C public static method Main(): int "
        "  try var v := A.X; return -1; "
        "  catch var ex: TypeInitializationException => do "
        "    if ex.InnerException is not null then return 2; end; "  // double-wrapped → 2
        "    return 55; "                                            // inner null → 55
        "  end end; "
        "end Main; end C;"), 55);
}

// §22.13: owner-thread tracking must NOT disturb ordinary (acyclic) cross-type
// lazy static init on a single thread — A depends on B, B on nothing; touching A
// initializes B then A and observes the fully-initialized values. Regression guard
// that adding the same-thread re-entrancy classification did not break the normal
// dependency chain. (This is also the rule-1 posture anchor: a same-thread acyclic
// chain proceeds; only a same-thread RE-ENTRY raises. A cross-domain in-progress
// touch is the §22.12 rule-1 blocking case, #1391 — not exercised here.)
BEZEL_TEST(static_init_cycle_backstop_1553,
           acyclic_cross_type_lazy_init_unaffected_spec_22_13) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class B public static readonly V: int := 20; end B; "
        "class A public static readonly W: int := B.V + 3; end A; "
        "class C public static method Main(): int => A.W; "  // 23
        "end C;"), 23);
}

// §22.13 rule 2: the synthesized backstop exception's message NAMES the re-entrant
// type (here 'A'). Read on the FIRST (synthesizing) access. Exact-equality against
// the codegen-fixed text keeps the "names the type" contract observable.
BEZEL_TEST(static_init_cycle_backstop_1553,
           synthesized_TIE_message_names_reentrant_type_spec_22_13) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kIntCycle +
        "class C public static method Main(): int "
        "  try var v := A.X; return -1; "
        "  catch var ex: TypeInitializationException => do "
        "    if ex.Message = \"The static initializer for type 'A' was re-entered "
        "during its own initialization on the same thread "
        "(static-initialization cycle)\" then return 55; else return 1; end; "
        "  end end; "
        "end Main; end C;"), 55);
}
