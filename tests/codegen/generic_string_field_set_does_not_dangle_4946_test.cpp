// Regression pin for issue #4946 — §4.6 (string is a runtime-managed,
// reference-counted value) / §28.9 (every owning location holds its own
// reference; the owner's disposal releases it) / §14.1 (a generic class body
// is lowered per instantiation), v0.307.
//
// THE DEFECT. A generic class `B<T>` storing a fresh string into a bare-`T`
// field from a method body (`method Set(v: T) _v := v;`) released the caller's
// string on the method-body store at `T = string`: the store is typed `T` in
// sema, and every string retain/release predicate on the class side was keyed
// on the sema type, so the caller's string the constructor had merely aliased
// was freed out from under it (heap-use-after-free in `bz_string_to_utf8z`).
//
// COVERED BY. The fix #4965 (commit a350f8b6, "balanced class-side ownership
// for bare-T fields instantiated at string") retains every store into a
// bare-`T` class member whose active substitution binds `T` to `string` — the
// generic-body `:=` / `??=`, the auto-property setter body, the primary-ctor
// positional store, the field initializer, a `ref U` parameter store at
// `U = string` — and the per-instantiation disposer releases it. d17a5f6e
// (which contains a350f8b6) is plain `v3` and ASan-clean on this file's
// verbatim probe.
//
// This file pins that behaviour with the issue's verbatim shape (a
// `Set(v: T) _v := v;` method called once on a string interpolation), the
// 200-iteration loop amplification that defeats the one-shot LSan vacuous
// pass (CLAUDE.md "Oracles that pass vacuously" — one-shot LSan reports only
// UNREACHABLE allocations, so a single run can leave the allocation
// reachable from a live stack slot and pass on both sides of a fix), the
// swap idiom that produced the issue's `free(): invalid pointer` abort, and
// the non-generic-twin accept control that demonstrates the same lowering
// path on a bare `string _v` was already correct — so a regression here
// cannot silently recover by a narrowing that breaks the working path.
//
// Rows that are leak-clean with the fix run on the plain AOT path AND under
// generated-code ASan+LSan (runAotChecked). Rows exercising the generic-body
// store run under generated-code ASan with LSan off (runAotCheckedLenient):
// the leaked old value is the recorded policy above, and what they pin is
// the absence of the use-after-free. Every row asserts exact stdout.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

void expectPlain(const char* src, const std::string& expected) {
    JitResult plain = runAot(src);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    BEZEL_EXPECT_EQ(plain.stdoutText, expected);
}

// Plain + ASan (LSan off): no use-after-free / double free / wild read.
void expectNoUaf(const char* src, const std::string& expected) {
    expectPlain(src, expected);
    JitResult checked = runAotCheckedLenient(src);
    BEZEL_EXPECT_TRUE(checked.ran);
    BEZEL_EXPECT_EQ(checked.exitCode, 0);
    BEZEL_EXPECT_EQ(checked.stdoutText, expected);
}

// Plain + ASan/LSan: the program must be memory-clean, leaks included.
void expectClean(const char* src, const std::string& expected) {
    expectPlain(src, expected);
    JitResult checked = runAotChecked(src);
    BEZEL_EXPECT_TRUE(checked.ran);
    BEZEL_EXPECT_EQ(checked.exitCode, 0);
    BEZEL_EXPECT_EQ(checked.stdoutText, expected);
}

}  // namespace

// ─── The issue's verbatim probe (UAF / silent wrong code) ──────────────────
// Pre-fix: bz_string_to_utf8z SIGSEGV / printed empty string — the
// method-body store released the caller's string, the next read dangled.
BEZEL_TEST(generic_string_field_set_does_not_dangle_4946, issue_repro_verbatim) {
    expectNoUaf(R"BZL(
module M;
public class B<T>
  private _v: T;
  public constructor(v: T) _v := v; end constructor;
  public method Set(v: T) _v := v; end Set;
  public method Get(): T => _v;
end B;
public class Program
  public static method Main(): int
    using var e := new B<string>("start");
    var i := 3;
    e.Set($"v{i}");
    Console.WriteLine(e.Get());   // expected v3
    return 0;
  end Main;
end Program;
)BZL",
                "v3\n");
}

// ─── 200-iter loop amplification (one-shot LSan vacuous-pass defence) ──────
// Pre-fix: SIGSEGV on a long loop; the loop is required to defeat the
// one-shot LSan vacuous-oracle class (CLAUDE.md "Oracles that pass
// vacuously" — one-shot leak repros).
BEZEL_TEST(generic_string_field_set_does_not_dangle_4946, set_loop_200) {
    expectNoUaf(R"BZL(
module M;
public class B<T>
  private _v: T;
  public constructor(v: T) _v := v; end constructor;
  public method Set(v: T) _v := v; end Set;
  public method Get(): T => _v;
end B;
public class Program
  public static method Main(): int
    using var e := new B<string>("start");
    for var i in 0..<200 do
      e.Set($"v{i}");
    end;
    Console.WriteLine(e.Get());   // expected v199
    return 0;
  end Main;
end Program;
)BZL",
                "v199\n");
}

// ─── The Put variant (the issue's `free(): invalid pointer` abort) ─────────
// The swap idiom is what the issue specifically reported: storing the OLD
// value into a `T` local (which holds no +1 of its own) and then freeing
// `_v` produces a UAF when the local goes out of scope. Pre-fix: abort
// with "free(): invalid pointer" (the local's reference slot was never
// accounted for; see #4965 commit body — making T locals refcounted is
// filed as #4980).
BEZEL_TEST(generic_string_field_set_does_not_dangle_4946, put_swap_variant_no_abort) {
    expectNoUaf(R"BZL(
module M;
public class B<T>
  private _v: T;
  public constructor(v: T) _v := v; end constructor;
  public method Put(v: T): T do var old: T := _v; _v := v; return old; end Put;
  public method Get(): T => _v;
end B;
public class Program
  public static method Main(): int
    using var e := new B<string>("start");
    var h := e.Put($"v1");     // first swap — returned "start"
    var g := e.Put($"v2");     // second swap — returned "v1"
    Console.WriteLine(h);
    Console.WriteLine(g);
    Console.WriteLine(e.Get());
    return 0;
  end Main;
end Program;
)BZL",
                "start\nv1\nv2\n");
}

// ─── Accept control — non-generic twin ─────────────────────────────────────
// A bare `string _v` field on a non-generic class with the SAME method
// shape. Must pass on pre-fix too: this pins that the lowering path for
// the working case (sema type is `string`, not `T`) has not regressed —
// so a regression on the generic-body rows above cannot silently recover
// by narrowing the compiler in a way that also breaks this control.
BEZEL_TEST(generic_string_field_set_does_not_dangle_4946, non_generic_twin_accept_control) {
    expectClean(R"BZL(
module M;
public class B
  private _v: string;
  public constructor(v: string) _v := v; end constructor;
  public method Set(v: string) _v := v; end Set;
  public method Get(): string => _v;
end B;
public class Program
  public static method Main(): int
    using var e := new B("start");
    var i := 3;
    e.Set($"v{i}");
    Console.WriteLine(e.Get());   // expected v3
    return 0;
  end Main;
end Program;
)BZL",
                "v3\n");
}
