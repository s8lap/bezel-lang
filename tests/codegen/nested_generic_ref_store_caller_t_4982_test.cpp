// Regression pin for issue #4982 — §14.1 (a generic method is specialized per
// instantiation; its type parameters belong to it) / §22.3 (a `ref` store is
// an assignment through an alias to the caller's location) / §4.6 + §28.9
// (string is reference-counted; every owning location holds its own
// reference), v0.307.
//
// THE DEFECT. A generic method called from inside another generic body with a
// type argument that names the ENCLOSING type parameter —
// `Util.Put<T>(ref _v, x)` inside `B<T>` — was specialized with the right
// LLVM binding (`U -> ptr`) but a stale sema binding `U -> T`: the call-site
// type argument was recorded without resolving it through the caller's active
// substitution, so inside `Util$Put$spec.1` `U` resolved to the bare,
// unbound `T`. Every sema-driven decision in the callee then saw an unresolved
// type parameter; in particular the `ref U` store at `U = string`
// (`x := v`) plain-stored without `bz_string_retain`, so the field aliased the
// caller's temporary string, which was freed after the call. Measured before
// the fix: SIGSEGV in `bz_string_to_utf8z` (issue base 1d40631f); at
// bb41c32f^ the same program prints an empty line for `e.Val`.
//
// FIXED BY bb41c32f (#4813), which resolves every call-site-supplied type —
// explicit type arguments, argument types and the result type — in the
// CALLER's substitution (`inCallerScope` in applyMethodGenericSubst) before
// binding the callee's parameters. Both rows below fail when that resolution
// is removed (MEASURED: 0/2 pass), so this file pins it.
//
// Oracles. Every row asserts exact stdout on the plain AOT path and under
// generated-code ASan with LSan off (runAotCheckedLenient). LSan is off
// because the `ref U` store at `U = string` is retain-only by the recorded
// policy of #4965 — the overwritten old value leaks, which is issue #4980;
// what these rows pin is the absence of the use-after-free. Move them to
// runAotChecked when #4980 is fixed. The 100-iteration loop makes each
// overwritten temporary a freed allocation the field would dangle into.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotCheckedLenient;

namespace {

// Plain + ASan (LSan off): no use-after-free / wild read, exact stdout.
void expectNoUaf(const char* src, const std::string& expected) {
    JitResult plain = runAot(src);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    BEZEL_EXPECT_EQ(plain.stdoutText, expected);
    JitResult checked = runAotCheckedLenient(src);
    BEZEL_EXPECT_TRUE(checked.ran);
    BEZEL_EXPECT_EQ(checked.exitCode, 0);
    BEZEL_EXPECT_EQ(checked.stdoutText, expected);
}

}  // namespace

// ─── The issue's verbatim repro: explicit `Put<T>` naming the caller's T ────
// Pre-fix: SIGSEGV in bz_string_to_utf8z (or an empty first line) — the
// `ref U` store in Util$Put$spec.1 took no reference at U = string.
BEZEL_TEST(nested_generic_ref_store_caller_t_4982,
           explicit_caller_t_type_arg_spec_14_1_22_3) {
    expectNoUaf(R"BZL(
module M;
public class Util
  public static method Put<U>(ref x: U, v: U)
    x := v;
  end Put;
end Util;
public class B<T>
  private _v: T;
  public constructor(v: T) _v := v; end constructor;
  public method Reset(x: T) Util.Put<T>(ref _v, x); end Reset;
  public property Val: T => _v;
end B;
public class Program
  public static method Main(): int
    var h := "a" + 1.ToString();
    using var e := new B<string>(h);
    for var i in 0..<100 do
      e.Reset("r" + i.ToString());
    end;
    Console.WriteLine(e.Val);
    Console.WriteLine(h);
    return 0;
  end Main;
end Program;
)BZL",
                "r99\na1\n");
}

// ─── The inference half of the issue: `Put(ref _v, x)` with U inferred ──────
// The issue names the argument-type inference path as carrying the same
// unresolved caller type; `U` is bound from `_v: T` / `x: T`, both caller-side
// types. Same program, same expected output.
BEZEL_TEST(nested_generic_ref_store_caller_t_4982,
           inferred_from_caller_t_args_spec_14_1_22_3) {
    expectNoUaf(R"BZL(
module M;
public class Util
  public static method Put<U>(ref x: U, v: U)
    x := v;
  end Put;
end Util;
public class B<T>
  private _v: T;
  public constructor(v: T) _v := v; end constructor;
  public method Reset(x: T) Util.Put(ref _v, x); end Reset;
  public property Val: T => _v;
end B;
public class Program
  public static method Main(): int
    var h := "a" + 1.ToString();
    using var e := new B<string>(h);
    for var i in 0..<100 do
      e.Reset("r" + i.ToString());
    end;
    Console.WriteLine(e.Val);
    Console.WriteLine(h);
    return 0;
  end Main;
end Program;
)BZL",
                "r99\na1\n");
}
