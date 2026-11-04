// Ownership / codegen — §8.6 / §12.1 / §17.13 / §17.15 / §27.5 (v0.355),
// fix-forward of ca822b0b (Refs #8095, narrow #6832): the callee escape walk
// (`ParamEscapeWalker`, src/codegen/LlvmBackend_Assign.cpp) must not answer
// "non-escaping" for a parameter it never SAW.
//
// THE DEFECT (MEASURED on staging a2fc2167). The walker had no arm for a
// §17.15 binding atom (`(var t := v; t)`), a §17.13 discard statement
// (`_ := s.Put(v);`), or the CALLEE of a call whose callee is itself a call
// (`s.Getter(v)()`), so an occurrence of the parameter there was skipped and
// the callee was cached as NOT storing it. It also walked a `virtual` member's
// own body although the body that runs is the receiver's override (§12.1). In
// every case the caller released the parameter's §8.6 box after the call
// (#2952 for a method; ca822b0b for a constructor) while a field still held
// it: a heap-use-after-free, 100 of 100 runs.
//
// THE FIX. Each of those kinds is walked, a dispatched (`virtual`, or
// non-`sealed` `override`) callee answers "every parameter escapes", and an
// expression or statement kind the walker does not model now ESCAPES instead
// of being skipped (§8.6: a runtime-managed box released early is a
// use-after-free; one kept is only a leak).
//
// ORACLES. Rows 1-6 are the escaping shapes, loop-amplified 100x inside the
// program, on `runAotCheckedLenient`: generated-code ASan is active (a UAF
// fails the row), LeakSanitizer is OFF because the conservative direction is a
// leak (#6827's field-held box). Each asserts the exact stdout `100`, so a
// wrong value read from a freed box fails it without ASan too. Rows 7-8 are
// CONTROLS that must STAY non-escaping: run on `runAotChecked` (LeakSanitizer
// ON), 100x, so a fix that over-reported escapes would leak and fail them.

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::checkedToolchainSkipped;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// (1) §17.15: a CONSTRUCTOR stores its parameter through a binding atom.
const char* kBindCtor =
    "module H01; "
    "public class C "
    "  public _saved: Object?; "
    "  public constructor(v: Object) "
    "    _saved := (var t := v; t); "
    "  end constructor; "
    "end C; "
    "public class Program "
    "  public static method Main(): int "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var c := new C(i + 1000); "
    "      if c._saved!.ToString() = (i + 1000).ToString() then n := n + 1; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (2) §17.15: a METHOD stores its parameter through a binding atom.
const char* kBindMethod =
    "module W08; "
    "public class Sink "
    "  public _saved: Object?; "
    "  public constructor() _saved := null; end constructor; "
    "end Sink; "
    "public class Program "
    "  public static method Keep(s: Sink, o: Object): int "
    "    s._saved := (var t := o; t); "
    "    return 1; "
    "  end Keep; "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var a := Keep(s, i + 1000); "
    "      if s._saved!.ToString() = (i + 1000).ToString() then n := n + a; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (3) §17.13: a discard statement whose value is a member call that stores
// the parameter.
const char* kDiscardMemberCall =
    "module H02; "
    "public class Sink "
    "  public _saved: Object?; "
    "  public constructor() _saved := null; end constructor; "
    "  public method Put(o: Object): int "
    "    _saved := o; "
    "    return 1; "
    "  end Put; "
    "end Sink; "
    "public class Program "
    "  public static method Keep(s: Sink, o: Object): int "
    "    _ := s.Put(o); "
    "    return 1; "
    "  end Keep; "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var a := Keep(s, i + 1000); "
    "      if s._saved!.ToString() = (i + 1000).ToString() then n := n + 1; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (4) §17.13: a discard statement whose value is a static call that stores
// the parameter.
const char* kDiscardStaticCall =
    "module W07; "
    "public class Sink "
    "  public _saved: Object?; "
    "  public constructor() _saved := null; end constructor; "
    "end Sink; "
    "public class Program "
    "  public static method Store(s: Sink, o: Object): int "
    "    s._saved := o; "
    "    return 1; "
    "  end Store; "
    "  public static method Keep(s: Sink, o: Object): int "
    "    _ := Store(s, o); "
    "    return 1; "
    "  end Keep; "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var a := Keep(s, i + 1000); "
    "      if s._saved!.ToString() = (i + 1000).ToString() then n := n + a; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (5) §12.1: the call binds `Base.Put`, whose body only returns 0, but the
// receiver's `override` runs and stores the parameter.
const char* kVirtualOverrideStores =
    "module W03; "
    "public class Base "
    "  public constructor() end constructor; "
    "  public virtual method Put(v: Object): int => 0; "
    "end Base; "
    "public class Keeper: Base "
    "  public _saved: Object?; "
    "  public constructor() _saved := null; end constructor; "
    "  public override method Put(v: Object): int "
    "    _saved := v; "
    "    return 1; "
    "  end Put; "
    "end Keeper; "
    "public class Program "
    "  public static method Main(): int "
    "    using var k := new Keeper(); "
    "    var b: Base := k; "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var a := b.Put(i + 1000); "
    "      if k._saved!.ToString() = (i + 1000).ToString() then n := n + a; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (6) The callee of the outer call is the inner call `s.Getter(o)`, which
// stores the parameter.
const char* kCalleeIsCall =
    "module W05; "
    "public class Sink "
    "  public _saved: Object?; "
    "  public constructor() _saved := null; end constructor; "
    "  public method Getter(o: Object): () -> int "
    "    _saved := o; "
    "    return lambda() => 1; "
    "  end Getter; "
    "end Sink; "
    "public class Program "
    "  public static method Keep(s: Sink, o: Object): int "
    "    return s.Getter(o)(); "
    "  end Keep; "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var a := Keep(s, i + 1000); "
    "      if s._saved!.ToString() = (i + 1000).ToString() then n := n + a; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (7) CONTROL: a discard and a binding atom that only READ the parameter (a
// type test, §8.6: "A type-test operand ... [is] not such [a boxing]
// position"). The box must still be released after each call.
const char* kControlBindDiscardRead =
    "module C01; "
    "public class Program "
    "  public static method Look(o: Object): int "
    "    _ := (if o is int then 1 else 0); "
    "    var n := (var t := 1; (if o is int then t else 0)); "
    "    return n; "
    "  end Look; "
    "  public static method Main(): int "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      n := n + Look(i + 1000); "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// (8) CONTROL: a `sealed override` that only reads its parameter, called
// through its own class. No further override can run (§12.4), so its body is
// the body that runs and the box must still be released.
const char* kControlSealedOverrideRead =
    "module C02; "
    "public class Base "
    "  public constructor() end constructor; "
    "  public virtual method Look(v: Object): int => 0; "
    "end Base; "
    "public class Leaf: Base "
    "  public constructor() end constructor; "
    "  public sealed override method Look(v: Object): int "
    "    => (if v is int then 1 else 0); "
    "end Leaf; "
    "public class Program "
    "  public static method Main(): int "
    "    using var k := new Leaf(); "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      n := n + k.Look(i + 1000); "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

}  // namespace

BEZEL_TEST(ownership_param_escape_unmodelled_kinds_8095, ctor_bind_atom_store_keeps_box_spec_17_15_8_6) {
    JitResult r = runAotCheckedLenient(kBindCtor);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}

BEZEL_TEST(ownership_param_escape_unmodelled_kinds_8095, method_bind_atom_store_keeps_box_spec_17_15_8_6) {
    JitResult r = runAotCheckedLenient(kBindMethod);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}

BEZEL_TEST(ownership_param_escape_unmodelled_kinds_8095, discard_member_call_store_keeps_box_spec_17_13_8_6) {
    JitResult r = runAotCheckedLenient(kDiscardMemberCall);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}

BEZEL_TEST(ownership_param_escape_unmodelled_kinds_8095, discard_static_call_store_keeps_box_spec_17_13_8_6) {
    JitResult r = runAotCheckedLenient(kDiscardStaticCall);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}

BEZEL_TEST(ownership_param_escape_unmodelled_kinds_8095, virtual_callee_override_store_keeps_box_spec_12_1_8_6) {
    JitResult r = runAotCheckedLenient(kVirtualOverrideStores);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}

BEZEL_TEST(ownership_param_escape_unmodelled_kinds_8095, callee_is_call_store_keeps_box_spec_8_6) {
    JitResult r = runAotCheckedLenient(kCalleeIsCall);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}

BEZEL_TEST(ownership_param_escape_unmodelled_kinds_8095, control_bind_discard_read_releases_box_spec_8_6) {
    JitResult r = runAotChecked(kControlBindDiscardRead);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}

BEZEL_TEST(ownership_param_escape_unmodelled_kinds_8095, control_sealed_override_read_releases_box_spec_12_1_8_6) {
    JitResult r = runAotChecked(kControlSealedOverrideRead);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}
