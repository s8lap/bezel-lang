// fix #4931 / §28.3 *Returning a deferred-activation frame* / §29.11 / §29.2
// (spec v0.307): the SYNC half of `recv.GetEnumerator()` over a SCOPE-BOUND
// owned local / `using var` carries a borrow of `recv` that outlives its
// source — the array enumerator stores a raw pointer to the source array
// (LlvmBackend_Assign.cpp arrenum: `{vtable, arr, idx, esz}`), and the
// source's auto-drop at scope exit reclaims the backing storage
// (§28.3 affine auto-drop / §28.10 using-var dispose). The result is
// the wild-SEGV measured for `using var en := Mk()` where `Mk(): owned
// IEnumerator<int> return xs.GetEnumerator()` over a `var xs: Array<int>`
// local — the wrapper's `Current` reads `arr` which is now a dead slot.
//
// The static check at `checkBorrowEscapesOnReturn` does NOT fire on this
// shape: the call's result type is a builtin `IEnumerator<T>` sentinel
// (Named with no declaration), so `isBorrowableType` answers false and
// the `returnedExprMayBorrow` gate skips the whole check. The targeted
// rejection is a NEW static check at `handleReturn`, mirroring the
// §28.3 / stdlib §4.5 / #2175 drain-view return-site check.
//
// Each positive test compiles the rejected program through ownership
// checking and asserts hasErrors() with a substring match on the
// diagnostic. Each negative test compiles through and runs under
// runAotChecked to confirm the admitted shapes still work. // per
// §28.3, §29.2, §29.11, #2443 (named-interface carve), #4931

#include <string>

#include "support/aot_support.hpp"        // runAotChecked — AOT + ASan/LSan
#include "support/ownership_support.hpp"  // parseAndCheckOwnership — diag pins
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

}  // namespace

// ===========================================================================
// (1) §28.3 / §29.11 / §29.2 — `return xs.GetEnumerator()` over a plain
// (non-using) LOCAL of declared heap-collection type `Array<int>` is
// REJECTED. The collection-literal initializer bypasses producerState's
// CollectionLiteralExpr arm (the literal is untyped — its type comes from
// the declaration), so `xs.state == Borrowed` and `everOwnedPlainLocals_`
// would not carry `xs`; the local is heap-allocated in spite of the
// classification, and the §28.3 affine auto-drop frees the backing at the
// callee's scope end. Detect by the local's DECLARED TYPE for a heap-
// collection head. // per §28.3, §15.4, #4931
// ===========================================================================
BEZEL_TEST(local_array_getenumerator_4931,
           return_local_array_getenumerator_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "public class Program "
        "  static method Mk(): owned IEnumerator<int> "
        "    var xs: Array<int> := [10, 20, 30]; "
        "    return xs.GetEnumerator(); "
        "  end Mk; "
        "  public static method Main(): int return 0; end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("cannot return `xs.GetEnumerator()`") !=
                std::string::npos &&
            d.message.find("scope-bound owned local") != std::string::npos &&
            d.message.find("§28.3") != std::string::npos) {
            found = true; break;
        }
    }
    BEZEL_EXPECT_TRUE(found);
}

// ===========================================================================
// (2) §28.3 / §29.11 / §29.2 — the same shape with a `using var` (the
// implicit-discharge tracked-owned case): REJECTED. The diagnostic's
// "using var" branch fires instead of the plain-owned branch. // per
// §28.3, §28.10, §29.11, #4931
// ===========================================================================
BEZEL_TEST(local_array_getenumerator_4931,
           return_using_var_array_getenumerator_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "public class Program "
        "  static method Mk(): owned IEnumerator<int> "
        "    using var xs := new Array<int>(); "
        "    xs.Add(10); xs.Add(20); xs.Add(30); "
        "    return xs.GetEnumerator(); "
        "  end Mk; "
        "  public static method Main(): int return 0; end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("cannot return `xs.GetEnumerator()`") !=
                std::string::npos &&
            d.message.find("using var") != std::string::npos &&
            d.message.find("§28.3") != std::string::npos) {
            found = true; break;
        }
    }
    BEZEL_EXPECT_TRUE(found);
}

// ===========================================================================
// (3) §28.3 CONTROL — `return xs.GetEnumerator()` over a PARAMETER
// (the §2175 / §4931 admitted shape): the parameter outlives the call,
// so the borrow is sound. Pre-fix this also compiled (it was correctly
// admitted); the negative control pins that the new check doesn't
// over-reject. // per §28.3, §29.11, #4931
// ===========================================================================
BEZEL_TEST(local_array_getenumerator_4931,
           return_parameter_array_getenumerator_admitted_control_spec_28_3) {
    JitResult r = runAotChecked(
        "module M; "
        "public class Program "
        "  static method Mk(xs: Array<int>): owned IEnumerator<int> "
        "    return xs.GetEnumerator(); "
        "  end Mk; "
        "  public static method Main(): int "
        "    var xs: Array<int> := [10, 20, 30]; "
        "    using var en := Program.Mk(xs); "
        "    var s := 0; "
        "    var n := 0; "
        "    while en.MoveNext() do "
        "      s := s + en.Current; "
        "      n := n + 1; "
        "    end; "
        "    Console.WriteLine($\"n={n} s={s}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "n=3 s=60\n");
}

// ===========================================================================
// (4) §28.3 CONTROL — `using var en := xs.GetEnumerator();` over a
// LOCAL heap-collection source, when the enumerator is NOT returned
// (consumed in the same scope): ADMITTED. The §29.2 for-desugaring
// pins the source for the enumerator's lifetime, and the source itself
// outlives the using-var. This is the everyday usage and must continue
// to compile. // per §28.3, §28.10, §29.2, #4931
// ===========================================================================
BEZEL_TEST(local_array_getenumerator_4931,
           local_array_getenumerator_used_locally_admitted_spec_29_2) {
    JitResult r = runAotChecked(
        "module M; "
        "public class Program "
        "  public static method Main(): int "
        "    var xs: Array<int> := [10, 20, 30]; "
        "    using var en := xs.GetEnumerator(); "
        "    var s := 0; "
        "    var n := 0; "
        "    while en.MoveNext() do "
        "      s := s + en.Current; "
        "      n := n + 1; "
        "    end; "
        "    Console.WriteLine($\"n={n} s={s}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "n=3 s=60\n");
}

// ===========================================================================
// (5) §28.3 CONTROL — `this._xs.GetEnumerator()`: the receiver is `this`,
// the chain is receiver-sourced, the call's return is a borrow whose
// source is the receiver. ADMITTED. // per §28.3, §29.11, #4931
// ===========================================================================
BEZEL_TEST(local_array_getenumerator_4931,
           return_this_field_getenumerator_admitted_control_spec_28_3) {
    JitResult r = runAotChecked(
        "module M; "
        "public class Owner "
        "  public owned _xs: Array<int>; "
        "  public constructor() _xs := new Array<int>(); end constructor; "
        "  public method View(): owned IEnumerator<int> "
        "    _xs.Add(10); _xs.Add(20); _xs.Add(30); "
        "    return _xs.GetEnumerator(); "
        "  end View; "
        "end Owner; "
        "public class Program "
        "  public static method Main(): int "
        "    using var o := new Owner(); "
        "    using var en := o.View(); "
        "    var s := 0; "
        "    var n := 0; "
        "    while en.MoveNext() do "
        "      s := s + en.Current; "
        "      n := n + 1; "
        "    end; "
        "    Console.WriteLine($\"n={n} s={s}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "n=3 s=60\n");
}
