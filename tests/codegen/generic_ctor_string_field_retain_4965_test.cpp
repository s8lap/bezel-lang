// Regression tests for issue #4965 — §4.6 (string is a runtime-managed,
// reference-counted value) / §28.9 (every owning location holds its own
// reference; the owner's disposal releases it) / §14.1 (a generic class body
// is lowered per instantiation), v0.307.
//
// THE DEFECT (heap-use-after-free + leak). A generic class `B<T>` storing a
// bare-`T` field from its own body (`constructor(v: T) Q := v;`,
// `method Put(x: T) _v := x;`) took NO reference at `T = string`: the store is
// typed `T` in sema, and every string retain/release predicate on the class
// side was keyed on the sema type. Two things then went wrong:
//   * an external `e.Q := x` on a `B<string>` receiver IS sema-typed `string`,
//     so it released the old value — which was the CALLER's string the
//     constructor had merely aliased. Measured on 1d40631f: the issue's repro
//     SIGSEGVs in bz_string_to_utf8z printing the caller's `h`;
//   * the specialized disposer never released the field, so the final value
//     leaked (36 B / 2 allocations for the 50-iteration loop).
//
// THE FIX. Every store into a bare-`T` class member whose active substitution
// binds `T` to `string` now retains (the generic-body `:=` / `??=`, the
// auto-property setter body, the positional primary-constructor store, the
// field initializer, a `ref U` parameter store at `U = string`), and the
// per-instantiation disposer releases it. A store from inside the generic body
// does NOT release the old value, because a `T` local there holds no +1 of its
// own (`var old := _v; _v := x; return old;`); that old value leaks instead —
// never a UAF. The follow-up that would release it is filed as #4980.
//
// Rows that are leak-clean with the fix run on the plain AOT path AND under
// generated-code ASan+LSan (runAotChecked). Rows exercising the generic-body
// store run under generated-code ASan with LSan off (runAotCheckedLenient):
// the leaked old value is the recorded policy above, and what they pin is the
// absence of the use-after-free. Every row asserts exact stdout.

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

// Plain + ASan/LSan: the program must be memory-clean, leaks included.
void expectClean(const char* src, const std::string& expected) {
    expectPlain(src, expected);
    JitResult checked = runAotChecked(src);
    BEZEL_EXPECT_TRUE(checked.ran);
    BEZEL_EXPECT_EQ(checked.exitCode, 0);
    BEZEL_EXPECT_EQ(checked.stdoutText, expected);
}

// Plain + ASan (LSan off): no use-after-free / double free / wild read.
void expectNoUaf(const char* src, const std::string& expected) {
    expectPlain(src, expected);
    JitResult checked = runAotCheckedLenient(src);
    BEZEL_EXPECT_TRUE(checked.ran);
    BEZEL_EXPECT_EQ(checked.exitCode, 0);
    BEZEL_EXPECT_EQ(checked.stdoutText, expected);
}

}  // namespace

// ─── The issue's repro (UAF) ──────────────────────────────────────────────
// Pre-fix: SIGSEGV in bz_string_to_utf8z — `e.Q := ...` released `h`.
BEZEL_TEST(generic_ctor_string_field_retain_4965, issue_repro_external_set_frees_caller) {
    expectClean(R"BZL(
module M;
public interface I<T>
  property Q: T;
end I;
public class B<T>: I<T>
  public constructor(v: T) Q := v; end constructor;
  public property Q: T;
end B;
public class Program
  public static method Main(): int
    var h := "a" + 1.ToString();
    using var e := new B<string>(h);
    e.Q := "x" + 2.ToString();
    Console.WriteLine(h);
    Console.WriteLine(e.Q);
    return 0;
  end Main;
end Program;
)BZL",
                "a1\nx2\n");
}

// Pre-fix: 36 B / 2 allocations leaked (the final value, never released).
BEZEL_TEST(generic_ctor_string_field_retain_4965, issue_repro_final_value_released) {
    expectClean(R"BZL(
module M;
public interface I<T>
  property Q: T;
end I;
public class B<T>: I<T>
  public constructor(v: T) Q := v; end constructor;
  public property Q: T;
end B;
public class Program
  public static method Main(): int
    using var e := new B<string>("start");
    for var i in 0..<50 do
      e.Q := "v" + i.ToString();
    end;
    Console.WriteLine(e.Q);
    return 0;
  end Main;
end Program;
)BZL",
                "v49\n");
}

// The repro amplified: 100 constructions, each constructor-store + external
// set. Pre-fix: "refcount over-release (string)" abort on the first pass.
BEZEL_TEST(generic_ctor_string_field_retain_4965, ctor_store_then_external_set_loop_100) {
    expectClean(R"BZL(
module M;
public class B<T>
  public constructor(v: T) Q := v; end constructor;
  public property Q: T;
end B;
public class Program
  public static method Main(): int
    for var i in 0..<100 do
      var h := "a" + i.ToString();
      using var e := new B<string>(h);
      e.Q := "x" + i.ToString();
      if i = 99 then Console.WriteLine(h); Console.WriteLine(e.Q); end;
    end;
    return 0;
  end Main;
end Program;
)BZL",
                "a99\nx99\n");
}

// ─── Instantiations the retain must NOT touch ─────────────────────────────
// `int`: the field is an i32; no retain, no release (a retain on an int would
// be a wild call). Clean before and after.
BEZEL_TEST(generic_ctor_string_field_retain_4965, int_instantiation_unchanged) {
    expectClean(R"BZL(
module M;
public class B<T>
  public constructor(v: T) Q := v; end constructor;
  public property Q: T;
end B;
public class Program
  public static method Main(): int
    using var e := new B<int>(5);
    for var i in 0..<100 do
      e.Q := e.Q + i;
    end;
    Console.WriteLine(e.Q);
    return 0;
  end Main;
end Program;
)BZL",
                "4955\n");
}

// A class instantiation: the field is a borrowed class reference, not a
// string; a string retain/release on it would corrupt the object.
BEZEL_TEST(generic_ctor_string_field_retain_4965, class_instantiation_unchanged) {
    expectClean(R"BZL(
module M;
public class Dog
  public property Name: string;
  public constructor(n: string) Name := n; end constructor;
end Dog;
public class B<T>
  public constructor(v: T) Q := v; end constructor;
  public property Q: T;
end B;
public class Program
  public static method Main(): int
    using var d := new Dog("rex");
    for var i in 0..<100 do
      using var e := new B<Dog>(d);
      if i = 99 then Console.WriteLine(e.Q.Name); end;
    end;
    Console.WriteLine(d.Name);
    return 0;
  end Main;
end Program;
)BZL",
                "rex\nrex\n");
}

// ─── Other constructing routes at `string` ────────────────────────────────
// A non-generic `E: B<string>` chaining into the generic constructor.
BEZEL_TEST(generic_ctor_string_field_retain_4965, derived_base_chain_loop_100) {
    expectClean(R"BZL(
module M;
public class B<T>
  public constructor(v: T) Q := v; end constructor;
  public property Q: T;
end B;
public class E: B<string>
  public constructor(s: string) base(s); end constructor;
end E;
public class Program
  public static method Main(): int
    for var i in 0..<100 do
      var h := "a" + i.ToString();
      using var e := new E(h);
      if i = 99 then Console.WriteLine(h); end;
    end;
    return 0;
  end Main;
end Program;
)BZL",
                "a99\n");
}

// The 5th of 5 (#4981): a primary-ctor positional field `(v: T)` reached
// through a `base(s)` chain in an explicit derived ctor
// (`E: B<string> { constructor(s: string) base(s); }`). Pre-fix on BASE
// (r20-c25 / d437bab4): SEGV in bz_string_to_utf8z — the new-site GEP+store
// for B's positional slot uses the GENERIC `class.B` struct (slot 2 typed
// `i32` from the T→i32 placeholder layout) while the actual allocation is
// `class.E$spec0` (slot 2 = `ptr`), so the 8-byte ptr write at offset 12 of
// `class.B`'s 16-byte layout overwrites the padding / part of slot 2 and
// the subsequent `e.Q` read dereferences garbage. Pre-fix on the producer's
// intermediate (#4965 / 703fffdb): SEGV in bz_string_release at dispose —
// the corrupted slot's i32 read then +1s an unrelated pointer. The fix is
// in `prePopulateBasePositionalFields` itself: (1) install the base's
// generic-parameter subst (`B.T → ptr`) from `t->baseTypeRefs[bi]` BEFORE
// `classStruct(baseTs)`, so the returned layout is the SPECIALIZED
// `class.B$spec0` (slot 2 = ptr, offset 16) instead of the GENERIC
// `class.B` (slot 2 = i32, offset 12) — the 8-byte ptr write lands in an
// 8-byte slot, the read returns the stored pointer; and (2) emit a +1
// retain at the store when the slot is `isStringStorageMember(fs)` /
// `isTypeParamStringStorageMember(fs)`, symmetric to the producer's #4965
// retain in `storePrimaryCtorPositionalArgs` — that helper returns early
// for the non-generic derived `E`, so the +1 has to come from this
// function or dispose's #4965 release over-releases the caller's string.
BEZEL_TEST(generic_ctor_string_field_retain_4965,
           base_clause_positional_primary_ctor_loop_50) {
    expectClean(R"BZL(
module M;
public class B<T>(v: T)
  public property Q: T => v;
end B;
public class E: B<string>
  public constructor(s: string) base(s); end constructor;
end E;
public class Program
  public static method Main(): int
    for var i in 0..<50 do
      var h := "a" + i.ToString();
      using var e := new E(h);
      if i = 49 then Console.WriteLine(h); Console.WriteLine(e.Q); end;
    end;
    return 0;
  end Main;
end Program;
)BZL",
                "a49\na49\n");
}

// A primary-constructor positional field `(value: T)`. Pre-fix: 3590 B / 200
// allocations leaked (the +1 producer argument was stored, never released).
BEZEL_TEST(generic_ctor_string_field_retain_4965, primary_positional_field_loop_100) {
    expectClean(R"BZL(
module M;
public class Box<T>(value: T)
  public property V: T => value;
end Box;
public class Program
  public static method Main(): int
    for var i in 0..<100 do
      using var b := new Box<string>("a" + i.ToString());
      if i = 99 then Console.WriteLine(b.V); end;
    end;
    return 0;
  end Main;
end Program;
)BZL",
                "a99\n");
}

// ─── Stores from inside the generic body (retain-only; #4980) ─────────────
// Pre-fix: SIGSEGV — the caller's temporary was stored unretained and read
// back after it had been released.
BEZEL_TEST(generic_ctor_string_field_retain_4965, generic_method_set_loop_100) {
    expectNoUaf(R"BZL(
module M;
public class B<T>
  private _v: T;
  public constructor(v: T) _v := v; end constructor;
  public method Put(x: T) _v := x; end Put;
  public property Val: T => _v;
end B;
public class Program
  public static method Main(): int
    var h := "a" + 1.ToString();
    using var e := new B<string>(h);
    for var i in 0..<100 do
      e.Put("p" + i.ToString());
    end;
    Console.WriteLine(e.Val);
    Console.WriteLine(h);
    return 0;
  end Main;
end Program;
)BZL",
                "p99\na1\n");
}

// The swap idiom is why the generic-body store must not release the old
// value: `old` is a `T` local holding no +1. Pre-fix: SIGSEGV.
BEZEL_TEST(generic_ctor_string_field_retain_4965, generic_swap_idiom_loop_100) {
    expectNoUaf(R"BZL(
module M;
public class B<T>
  private _v: T;
  public constructor(v: T) _v := v; end constructor;
  public method Swap(x: T): T
    var old := _v;
    _v := x;
    return old;
  end Swap;
  public property Val: T => _v;
end B;
public class Program
  public static method Main(): int
    var h := "a" + 1.ToString();
    using var e := new B<string>(h);
    for var i in 0..<100 do
      var o := e.Swap("v" + i.ToString());
      if i = 99 then Console.WriteLine(o); end;
    end;
    Console.WriteLine(e.Val);
    Console.WriteLine(h);
    return 0;
  end Main;
end Program;
)BZL",
                "v98\nv99\na1\n");
}

// `T?` at `string`: the nullable slot is still a string pointer. Pre-fix:
// SIGSEGV reading the value after a later `Set`.
BEZEL_TEST(generic_ctor_string_field_retain_4965, nullable_type_param_field_loop_100) {
    expectNoUaf(R"BZL(
module M;
public class B<T>
  private _v: T?;
  public constructor(v: T?) _v := v; end constructor;
  public property V: T? => _v;
  public method Set(x: T?) _v := x; end Set;
end B;
public class Program
  public static method Main(): int
    var h := "a" + 1.ToString();
    using var e := new B<string>(h);
    for var i in 0..<100 do
      e.Set("n" + i.ToString());
    end;
    e.Set(null);
    Console.WriteLine(h);
    Console.WriteLine(e.V ?? "none");
    e.Set("z" + 7.ToString());
    Console.WriteLine(e.V ?? "none");
    return 0;
  end Main;
end Program;
)BZL",
                "a1\nnone\nz7\n");
}

// A generic class holding another at the same `T` (`W<T>` → `B<T>`): the inner
// instantiation's substitution is reached through the outer one's.
// Pre-fix: SIGSEGV.
BEZEL_TEST(generic_ctor_string_field_retain_4965, nested_generic_owner_loop_100) {
    expectNoUaf(R"BZL(
module M;
public class B<T>
  public constructor(v: T) Q := v; end constructor;
  public property Q: T;
end B;
public class W<T>
  private owned _inner: B<T>;
  public constructor(v: T) _inner := new B<T>(v); end constructor;
  public method Set(v: T) _inner.Q := v; end Set;
  public property Get: T => _inner.Q;
end W;
public class Program
  public static method Main(): int
    var h := "a" + 1.ToString();
    for var i in 0..<100 do
      using var w := new W<string>(h);
      w.Set("w" + i.ToString());
      if i = 99 then Console.WriteLine(w.Get); end;
    end;
    Console.WriteLine(h);
    return 0;
  end Main;
end Program;
)BZL",
                "w99\na1\n");
}

// A store from a lambda body inside the generic method. Pre-fix: SIGSEGV.
BEZEL_TEST(generic_ctor_string_field_retain_4965, lambda_body_set_loop_100) {
    expectNoUaf(R"BZL(
module M;
public class B<T>
  private _v: T;
  public constructor(v: T) _v := v; end constructor;
  public method Later(x: T)
    var f := lambda() => do _v := x; end;
    f();
  end Later;
  public property Val: T => _v;
end B;
public class Program
  public static method Main(): int
    var h := "a" + 1.ToString();
    using var e := new B<string>(h);
    for var i in 0..<100 do
      e.Later("l" + i.ToString());
    end;
    Console.WriteLine(e.Val);
    Console.WriteLine(h);
    return 0;
  end Main;
end Program;
)BZL",
                "l99\na1\n");
}

// A `ref U` parameter store at `U = string` (§22.3: the store through the
// alias is an ordinary assignment to the caller's slot). Pre-fix: SIGSEGV.
BEZEL_TEST(generic_ctor_string_field_retain_4965, generic_ref_param_store_loop_100) {
    expectNoUaf(R"BZL(
module M;
public class Util
  public static method Put<U>(ref x: U, v: U)
    x := v;
  end Put;
end Util;
public class Program
  public static method Main(): int
    var h := "a" + 1.ToString();
    for var i in 0..<100 do
      Util.Put<string>(ref h, "r" + i.ToString());
    end;
    Console.WriteLine(h);
    return 0;
  end Main;
end Program;
)BZL",
                "r99\n");
}
