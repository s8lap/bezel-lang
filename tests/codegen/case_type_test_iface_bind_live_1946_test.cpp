// Regression for #1946 / §19.3/§19.5/§19.6/§12.9: a LIVE interface-scrutinee
// type-test arm must be ACCEPTED and must match at runtime with the PRECISE
// (per-monomorphization, #1843) semantics.
//
// #1927 added a static check (`typeTestInterfaceBindAmbiguous`) that rejected an
// interface-scrutinee typed binding `var x: C<A>` whenever a *second*
// instantiation of `C` also implemented the scrutinee interface — on the premise
// that the runtime type-test ERASES generic arguments, so a `C<Q≠A>` value would
// bind at the wrong static type `C<A>`. #1843 (per-monomorphization RTTI
// descriptors) then made the runtime `is C<A>` test PRECISE: it honors the type
// argument and matches ONLY genuine `C<A>` values. That falsified the erasure
// premise — the rejected arms are LIVE and sound — so #1946 narrowed the check
// to dead-arm-only (a pattern instantiation that provably cannot conform).
//
// This test pins the runtime behavior the static admission must permit:
//   * `C<T>: IMarker<T>, IMarker<int>`, scrutinee `IMarker<int>`, arm
//     `var x: C<string>`. `C<string>` conforms to `IMarker<int>` via the GROUND
//     clause, so a `C<string>` value flows in and MATCHES (LIVE).
//   * A `C<int>` value (also an `IMarker<int>`, via `T=int`) does NOT match the
//     `C<string>` arm — the runtime is PRECISE, not erased (#1843), so no wrong
//     bind occurs. It falls through to `_`.
//
// FAILS WITHOUT the #1946 narrowing: the program never compiles (#1927 rejects
// the `var x: C<string>` arm), so runAot yields ran=false and the -999 sentinel.
// PASSES WITH: exit 73 = 7 (C<string> matches) * 10 + 3 (C<int> falls to `_`).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// A method `Classify(m: IMarker<int>)` returns 7 iff `m` is a `C<string>`, else
// 3. `Main` feeds it a `C<string>` (must match → 7) and a `C<int>` (must NOT
// match the `C<string>` arm → 3), folding both into one 8-bit exit code (73).
int run() {
    auto r = runAot(
        "module M;\n"
        "interface IMarker<T>\nend IMarker;\n"
        "sealed class C<T> : IMarker<T>, IMarker<int>\nend C;\n"
        "class Program\n"
        "  public static method Classify(m: IMarker<int>): int\n"
        "    var n := case m of\n"
        "      var x: C<string> => 7,\n"
        "      _ => 3\n"
        "    end;\n"
        "    return n;\n"
        "  end Classify;\n"
        "  public static method Main(): int\n"
        "    using var a := new C<string>();\n"
        "    using var b := new C<int>();\n"
        "    return Program.Classify(a) * 10 + Program.Classify(b);\n"
        "  end Main;\n"
        "end Program;\n");
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// The live implemented-twice arm is admitted AND runs with precise (#1843)
// matching: C<string> matches (7), C<int> does not (3) → 73.
BEZEL_TEST(case_type_test_iface_bind_live_1946,
           implemented_twice_live_arm_precise_match) {
    BEZEL_EXPECT_EQ(run(), 73);
}
