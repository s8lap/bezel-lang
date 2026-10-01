// fix #4673 (stdlib §6.5, §24; spec §22.8, §17.2, v0.307): the `lhs += h`
// rewrites to `lhs.Add(h)` desugaring (§17.2) applies to `+=` on a plain
// `readonly` field typed `EventSource<T>` (or `AsyncEventSource<T>` /
// `SharedEventSource<T>`), not only to the legacy `event` member declaration
// form. The readonly field carries a field-initializer assignment (step 2 of
// construction), so a constructor-body `+=` is a SECOND assignment under
// §22.8's single-assignment rule and was rejected as
// "cannot assign to readonly field `_beep` more than once" — but the
// desugaring means `+=` is NOT a real assignment; it is a subscription call
// that neither reads nor writes the field's value.
//
// Cases:
// 1. `readonly EventSource<int>` field in constructor body — the l6_5_0009
//    shape. Pre-fix: single-assignment reject. Post-fix: desugars to
//    `.Add()`, lambda param inferred as `int`, program runs and prints `7`.
// 2. `readonly EventSource<int>` field in a method body — the l6_5_0007
//    shape. Pre-fix: no error (no second assignment concern in method body).
//    Post-fix: same. Pins that the fix does not break the already-working
//    method-body path.
// 3. `readonly AsyncEventSource<int>` field in constructor body — async
//    twin. Pre-fix: single-assignment reject. Post-fix: desugars to
//    `.Add()`, lambda param inferred as `int`, handler returns `Task`.
// 4. `readonly SharedEventSource<int>` field in constructor body —
//    capture-free twin. Pre-fix: single-assignment reject. Post-fix:
//    desugars to `.Add()`, lambda param inferred as `int`.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

const bezel::Diagnostic* findDiag(const bezel::test::JitResult& r,
                                   const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return &d;
    return nullptr;
}

}  // namespace

// Case 1: l6_5_0009 shape — `readonly EventSource<int>` in ctor body.
// Pre-fix: "cannot assign to readonly field `_beep` more than once".
// Post-fix: desugars to `.Add()`, prints `7`.
BEZEL_TEST(event_source_readonly_field_plus_equals_rewrite_4673,
           readonly_eventsource_field_ctor_body_plus_equals_l6_5_0009) {
    auto r = runAot(
        "module M; import System; "
        "class Beeper "
        "  private owned readonly _beep: EventSource<int> := new(); "
        "  public constructor() "
        "    _beep += lambda(n) => Console.WriteLine(n); "
        "  end constructor; "
        "  public method Fire(n: int) _beep.Raise(n); end Fire; "
        "end Beeper; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Beeper(); "
        "    b.Fire(7); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("7") != std::string::npos);
}

// Case 2: l6_5_0007 shape — `readonly EventSource<int>` in method body.
// Pre-fix: compiles clean. Post-fix: same. Regression guard.
BEZEL_TEST(event_source_readonly_field_plus_equals_rewrite_4673,
           readonly_eventsource_field_method_body_plus_equals_l6_5_0007) {
    auto r = runAot(
        "module M; import System; "
        "class Bus "
        "  private owned readonly _e: EventSource<int> := new(); "
        "  public property E: IEvent<int> => _e; "
        "  public constructor() end constructor; "
        "  public method AddLate() "
        "    _e += lambda(_) => Console.WriteLine(\"late\"); "
        "  end AddLate; "
        "  public method Fire(n: int) _e.Raise(n); end Fire; "
        "end Bus; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Bus(); "
        "    using var sub := b.E.Subscribe(lambda(_) => b.AddLate()); "
        "    b.Fire(1); "
        "    Console.WriteLine(\"mid\"); "
        "    b.Fire(2); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    std::string expect = "mid\nlate";
    BEZEL_EXPECT_TRUE(r.stdoutText.find("mid") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("late") != std::string::npos);
}

// Case 3: `readonly AsyncEventSource<int>` in constructor body.
// Pre-fix: single-assignment reject. Post-fix: desugars to `.Add()`.
// Runtime: AsyncEventSource Raise is async — compile-only check is sufficient
// for this sema-level fix; full async codegen is a separate concern.
BEZEL_TEST(event_source_readonly_field_plus_equals_rewrite_4673,
           readonly_async_eventsource_field_ctor_body_no_reject) {
    auto r = runAot(
        "module M; import System; "
        "class AsyncBeeper "
        "  private owned readonly _beep: AsyncEventSource<int> := new(); "
        "  public constructor() "
        "    _beep += lambda(n) => Console.WriteLine(n); "
        "  end constructor; "
        "end AsyncBeeper; "
        "class C "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    // The sema fix: no readonly single-assignment reject
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Case 4: `readonly SharedEventSource<int>` in constructor body.
// Pre-fix: single-assignment reject. Post-fix: desugars to `.Add()`.
// Compile-only: SharedEventSource Raise is cross-domain (separate codegen).
BEZEL_TEST(event_source_readonly_field_plus_equals_rewrite_4673,
           readonly_shared_eventsource_field_ctor_body_no_reject) {
    // SharedEventSource<T> requires a capture-free handler: a static method
    // reference or a non-capturing lambda. Use a static method here.
    auto r = runAot(
        "module M; import System; "
        "class SharedBeeper "
        "  private owned readonly _beep: SharedEventSource<int> := new(); "
        "  public constructor() "
        "    _beep += SharedBeeper.Handle; "
        "  end constructor; "
        "  private static method Handle(n: int): ()\n"
        "    do end;\n"
        "  end Handle; "
        "end SharedBeeper; "
        "class C "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    // The sema fix: no readonly single-assignment reject
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
