// §28.10 admitted-no-op `using var` binding of a VALUE-substituted owned-marker
// producer (spec §28.1/§28.10, v0.196; issue #1817, sub-task E3).
//
// §28.1 (v0.196) fixes the `owned` marker on a bare type parameter per
// instantiation; §28.10's uniform admissibility then guarantees that a
// marker-keyed BINDING form written under the class assumption — `using var y
// := producer()` where `producer` is declared to return `owned T` — stays
// ADMISSIBLE with a NO-OP disposal effect at a value/runtime-managed
// substitution. Concretely: `using var n := f.Make(5)` where `Make(owned x: T):
// owned T` and `f: Factory<int>` must COMPILE, RUN, and dispose NOTHING — the
// erased int carries no disposal obligation (§28.9), so codegen must emit no
// disposer at scope end (no spurious dispose, no double-free). The SAME source
// at a CLASS substitution (`Factory<Conn>`, Conn : IDisposable) must still
// dispose the owned Conn EXACTLY ONCE. E1's erasure must not turn the value
// binding into a hard reject, nor leave a spurious disposer that mis-disposes a
// plain value (a dynamic dispose of an erased int flips r.ran under ASan/LSan).
//
// Codegen is a clean no-op here on HEAD (verified end-to-end): for a CallExpr-
// initialized inferred `using var`, classOfLocal returns null, and
// usingVarOwnsClassObjectDynamically gates its dynamic dispose on
// isClassOrInterfaceRef of the per-instantiation SUBSTITUTED result type —
// false for int/string, true for Conn. These tests lock that in.
//
// Receiver form: each test uses a NAMED (bound) receiver `using var f := new
// Factory<T>()` rather than the unbound-temporary form `new Factory<T>().Make(
// ...)`. The temporary form trips a SEPARATE, pre-existing leak — an unbound
// receiver temporary of a destructorless class whose method returns owned is
// not reclaimed (reproduces non-generically; issue #1859) — which is orthogonal
// to the erasure no-op under test. The named form isolates exactly the value-
// substitution `using var` binding behavior. (The sibling `out using var` /
// `out owned var` call-site forms at a value substitution currently mis-dispose
// — issue #1860 — and are deliberately NOT asserted here; this file pins the
// binding form the ruling's uniform admissibility makes a clean no-op.)

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"  // runAotChecked
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

}  // namespace

// ===========================================================================
// (1) VALUE substitution T=int — `using var n := f.Make(5)`. The `owned T`
// return erases to a plain int (§28.1 v0.196); the `using var` binding is
// admissible (§28.10) with a NO-OP scope-end disposal (§28.9). It must compile,
// run, round-trip the value, and be ASan/LSan-clean — a spurious dynamic dispose
// of the erased int would flip r.ran (bz_obj_is_string on a non-pointer).
// `var v := n` copies the int out of the `using var` (a plain read, no move) so
// the round-trip value rides an 8-bit exit code — platform-independent, and it
// also proves the `using var` int is freely readable. // per §28.10 v0.196
// ===========================================================================
BEZEL_TEST(owned_bare_t_using_var_binding_1817,
           value_int_using_var_is_noop_spec_28_10) {
    JitResult r = runAotChecked(
        "module M; "
        "class Factory<T> "
        "  public method Make(owned x: T): owned T return x; end Make; "
        "end Factory; "
        "class P "
        "  public static method Main(): int "
        "    using var f := new Factory<int>(); "
        "    using var n := f.Make(5); "
        "    var v := n; "
        "    return v; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);            // no spurious dispose, no crash
    BEZEL_EXPECT_EQ(r.exitCode, 5);      // the erased value round-trips
}

// ===========================================================================
// (2) VALUE substitution T=string (runtime-managed, §28.9) — `using var s :=
// f.Make("hi")` binds an erased string producer; the scope-end disposal is a
// no-op. Runtime-managed values are trivial-dispose; the only observable effect
// of `using var` is the reassignability lock. Must run clean under ASan/LSan (a
// stray release/retain imbalance on the string would flip r.ran). // per §28.10
// ===========================================================================
BEZEL_TEST(owned_bare_t_using_var_binding_1817,
           value_string_using_var_is_noop_spec_28_10) {
    JitResult r = runAotChecked(
        "module M; "
        "class Factory<T> "
        "  public method Make(owned x: T): owned T return x; end Make; "
        "end Factory; "
        "class P "
        "  public static method Main(): int "
        "    using var f := new Factory<string>(); "
        "    using var s := f.Make(\"hi\"); "
        "    Console.WriteLine(s); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // Substring (not "hi\n"): Console.NewLine is platform-dependent.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("hi") != std::string::npos);
}

// ===========================================================================
// (3) CLASS substitution T=Conn — the control. `using var c := f.Make(new
// Conn(7))` binds the owned Conn the class-substituted `Make` transfers, and
// disposes it EXACTLY ONCE at scope end. runAotChecked: erasing here (dropping
// the obligation) would LEAK — the destructor would not run and LSan would flip
// r.ran; a double dispose would flip it too. This pins that the value-case no-op
// does not weaken the class case. // per §28.10 v0.196
// ===========================================================================
BEZEL_TEST(owned_bare_t_using_var_binding_1817,
           class_conn_using_var_disposes_once_spec_28_10) {
    JitResult r = runAotChecked(
        "module M; "
        "class Conn : IDisposable "
        "  public Id: int; "
        "  public constructor(id: int) Id := id; end constructor; "
        "  destructor Console.WriteLine(\"D\" + Id.ToString()); end destructor; "
        "end Conn; "
        "class Factory<T> "
        "  public method Make(owned x: T): owned T return x; end Make; "
        "end Factory; "
        "class P "
        "  public static method Main(): int "
        "    using var f := new Factory<Conn>(); "
        "    using var c := f.Make(new Conn(7)); "
        "    Console.WriteLine(\"body\"); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);                                   // no leak, no double-free
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D7"), 1);   // disposed once
    // Scope-end disposal: the destructor runs after the body statement.
    const std::size_t body = r.stdoutText.find("body");
    const std::size_t disp = r.stdoutText.find("D7");
    BEZEL_EXPECT_TRUE(body != std::string::npos);
    BEZEL_EXPECT_TRUE(disp != std::string::npos);
    BEZEL_EXPECT_TRUE(body < disp);
}
