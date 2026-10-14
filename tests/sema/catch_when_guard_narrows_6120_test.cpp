// Spec-derived regression tests for issue #6120 — a `catch` arm's `when` guard
// narrows its arm's body.
//
// THE RULE. Spec §7.4 (v0.338) establishes a narrowing by a null test or a
// successful `is` test "in an `if`/`elif` condition …, a `while` condition,
// or a `when` guard (§19.4, narrowing that arm's body)". A catch arm's guard
// is that §19.4 guard — §4.1: "a `case`/`catch` arm's `when` guard (§19.4)";
// §31.2.1: "Each pattern may be followed by an optional `when Expression`
// guard. … Inside the guard, the bound variable (if any) is in scope." §7.4
// also says "`not (x is T)` has the same truth value as `x is not T` but
// narrows neither branch".
//
// WHAT WAS BROKEN. The TryStmt handler type-checked a catch arm's guard and
// then ignored it (`if (arm.guard) visitExpr(*arm.guard);`), the same omission
// #6082 fixed for the arms of a `case`. MEASURED on 6d8c82eb
// with the real `bezelc`: every accept row below was rejected — `no member
// `Code` on `Exception``, `no member `y` on type `A``, or the §5.1 conversion
// error for `var m: MyErr := c`.
//
// The carrier here is a USER exception class (and a user class local), so the
// rows observe the guard's narrowing by itself. The built-in pair
// (`catch var c: Exception when c is IOException => … var io: IOException :=
// c`) compiled before through the unchecked exception downcast that #6023
// removes; its rows live with that fix
// (builtin_exception_downcast_not_implicit_6023_test.cpp).
//
// The reject controls are the limits #6082 pins for a `case` arm: `or` and
// `not`-composition narrow nothing, an assignment in the body ends the fact,
// the fact is the ARM's (not the next arm's), and it does not survive the
// construct.
//
// Every row is on `runAot` / `runAotChecked`; the accept rows compare stdout
// and take the guard-true and the guard-false path, the headline ones looped.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

constexpr const char* kPrelude = R"bzl(module M6120;
public class MyErr: Exception
  public constructor(m: string, c: int) base(m); Code := c; end constructor;
  public Code: int;
end MyErr;
public class OtherErr: Exception
  public constructor(m: string) base(m); end constructor;
  public Tag: int := 7;
end OtherErr;
public class A
  public constructor() end constructor;
  public x: int := 1;
end A;
public class B: A
  public constructor() end constructor;
  public y: int := 9;
end B;
)bzl";

}  // namespace

// THE ISSUE'S SHAPE, on a user exception class so the narrowing is observable
// on its own: `when c is MyErr` narrows the catch binding in that arm's body, so
// `c.Code` reads `MyErr`'s field. Looped; all three throw paths are taken.
BEZEL_TEST(sema_catch_when_guard_narrows_6120,
           type_test_guard_narrows_catch_binding_in_arm_body_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method Th(k: int)
    if k = 0 then throw new MyErr("m", 41); end if;
    if k = 1 then throw new OtherErr("o"); end if;
    throw new Exception("exc");
  end Th;
  public static method T(k: int): int
    try Th(k); catch var c: Exception when c is MyErr => do return c.Code; end, var c2: Exception => do return -1; end end try;
    return -2;
  end T;
  public static method Main(): int
    var t := 0;
    for var i in 0..200 do t := t + T(0) + T(1) + T(2); end for;
    Console.WriteLine($"{T(0)} {T(1)} {T(2)} {t}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("41 -1 -1 7839\n"));
}

// The same program under the generated-code sanitizer.
BEZEL_TEST(sema_catch_when_guard_narrows_6120,
           type_test_guard_narrowed_catch_binding_is_memory_clean_spec_7_4) {
    auto r = runAotChecked(std::string(kPrelude) + R"bzl(public class P
  public static method Th(k: int)
    if k = 0 then throw new MyErr("m", 41); end if;
    if k = 1 then throw new OtherErr("o"); end if;
    throw new Exception("exc");
  end Th;
  public static method T(k: int): int
    try Th(k); catch var c: Exception when c is MyErr => do return c.Code; end, var c2: Exception => do return -1; end end try;
    return -2;
  end T;
  public static method Main(): int
    var t := 0;
    for var i in 0..200 do t := t + T(0) + T(1) + T(2); end for;
    Console.WriteLine($"{T(0)} {T(1)} {T(2)} {t}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("41 -1 -1 7839\n"));
}

// The narrowed binding as a call argument and a typed initializer.
BEZEL_TEST(sema_catch_when_guard_narrows_6120,
           type_test_guard_narrowed_catch_binding_is_an_argument_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method Th(k: int)
    if k = 0 then throw new MyErr("m", 41); end if;
    if k = 1 then throw new OtherErr("o"); end if;
    throw new Exception("exc");
  end Th;
  public static method Take(e: MyErr): int => e.Code + 1;
  public static method T(k: int): int
    try Th(k); catch var c: Exception when c is MyErr => do var m: MyErr := c; return Take(c) + m.Code; end, var c2: Exception => do return -1; end end try;
    return -2;
  end T;
  public static method Main(): int
    var t := 0;
    for var i in 0..200 do t := t + T(0) + T(1) + T(2); end for;
    Console.WriteLine($"{T(0)} {T(1)} {T(2)} {t}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("83 -1 -1 16281\n"));
}

// `and`: both operands hold in the body — the type test and a null test of
// an unrelated local.
BEZEL_TEST(sema_catch_when_guard_narrows_6120,
           and_joined_guard_narrows_type_and_nullability_in_catch_arm_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method Th(k: int)
    if k = 0 then throw new MyErr("m", 41); end if;
    if k = 1 then throw new OtherErr("o"); end if;
    throw new Exception("exc");
  end Th;
  public static method T(k: int, s: string?): int
    try Th(k); catch var c: Exception when c is MyErr and s <> null => do return c.Code + s.Length; end, var c2: Exception => do return -1; end end try;
    return -2;
  end T;
  public static method Main(): int
    Console.WriteLine($"{T(0, "ab")} {T(0, null)} {T(1, "ab")}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("43 -1 -1\n"));
}

// The guard tests a local of the enclosing method, not the binding.
BEZEL_TEST(sema_catch_when_guard_narrows_6120,
           guard_narrows_an_outer_local_in_catch_arm_body_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method Th(k: int)
    if k = 0 then throw new MyErr("m", 41); end if;
    if k = 1 then throw new OtherErr("o"); end if;
    throw new Exception("exc");
  end Th;
  public static method T(k: int, o: A): int
    try Th(k); catch Exception when o is B => do return o.y; end, Exception => do return -1; end end try;
    return -2;
  end T;
  public static method Main(): int
    Console.WriteLine($"{T(0, new B())} {T(0, new A())}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 -1\n"));
}

// REJECT. `or` proves neither operand.
BEZEL_TEST(sema_catch_when_guard_narrows_6120,
           or_joined_guard_narrows_nothing_in_catch_arm_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method Th(k: int)
    if k = 0 then throw new MyErr("m", 41); end if;
    if k = 1 then throw new OtherErr("o"); end if;
    throw new Exception("exc");
  end Th;
  public static method T(k: int): int
    try Th(k); catch var c: Exception when c is MyErr or c is OtherErr => do return c.Code; end end try;
    return -2;
  end T;
  public static method Main(): int
    Console.WriteLine("RAN");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `Code` on `Exception`"));
}

// REJECT. §7.4: "`not (x is T)` … narrows neither branch".
BEZEL_TEST(sema_catch_when_guard_narrows_6120,
           not_composed_guard_narrows_nothing_in_catch_arm_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method Th(k: int)
    if k = 0 then throw new MyErr("m", 41); end if;
    if k = 1 then throw new OtherErr("o"); end if;
    throw new Exception("exc");
  end Th;
  public static method T(k: int): int
    try Th(k); catch var c: Exception when not (c is not MyErr) => do return c.Code; end end try;
    return -2;
  end T;
  public static method Main(): int
    Console.WriteLine("RAN");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `Code` on `Exception`"));
}

// REJECT. An assignment in the body ends the fact.
BEZEL_TEST(sema_catch_when_guard_narrows_6120,
           guard_fact_is_dropped_when_the_body_reassigns_the_binding_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method Th(k: int)
    if k = 0 then throw new MyErr("m", 41); end if;
    if k = 1 then throw new OtherErr("o"); end if;
    throw new Exception("exc");
  end Th;
  public static method T(k: int, alt: Exception): int
    try Th(k); catch var c: Exception when c is MyErr => do c := alt; return c.Code; end end try;
    return -2;
  end T;
  public static method Main(): int
    Console.WriteLine("RAN");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `Code` on `Exception`"));
}

// REJECT. The fact is the ARM's: the next arm is reached when the guard was
// false.
BEZEL_TEST(sema_catch_when_guard_narrows_6120,
           guard_fact_does_not_reach_the_next_catch_arm_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method Th(k: int)
    if k = 0 then throw new MyErr("m", 41); end if;
    if k = 1 then throw new OtherErr("o"); end if;
    throw new Exception("exc");
  end Th;
  public static method T(k: int): int
    try Th(k); catch var c: Exception when c is MyErr => do Console.Write(""); end, var c: Exception => do return c.Code; end end try;
    return -2;
  end T;
  public static method Main(): int
    Console.WriteLine("RAN");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `Code` on `Exception`"));
}

// REJECT. The fact ends with the arm.
BEZEL_TEST(sema_catch_when_guard_narrows_6120,
           guard_fact_does_not_survive_the_try_statement_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method Th(k: int)
    if k = 0 then throw new MyErr("m", 41); end if;
    if k = 1 then throw new OtherErr("o"); end if;
    throw new Exception("exc");
  end Th;
  public static method T(k: int, o: A): int
    try Th(k); catch Exception when o is B => do Console.Write(""); end end try;
    return o.y;
  end T;
  public static method Main(): int
    Console.WriteLine("RAN");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `y` on type `A`"));
}

// ---------------------------------------------------------------------------
// A guard-narrowed catch body is invalidated like an `if`-narrowed one (§7.4
// "How narrowing is invalidated", §12.7, v0.338). These rows pin the shapes of
// #6113 / #6114 / #6115 REACHED THROUGH A CATCH GUARD — each compiled on the
// first version of this fix and read a `B` field off an `A` (or dereferenced
// null) — and the accept rows that sit next to them. They share one driver.

namespace {

JitResult runGuarded(const char* body) {
    return runAot(std::string(kPrelude) + R"bzl(public class P
  public static method Rs(ref q: A, alt: A): bool
    q := alt;
    return true;
  end Rs;
  public static method Ns(ref s: string?): bool
    s := null;
    return true;
  end Ns;
  public static method Th(k: int)
    throw new Exception("exc");
  end Th;
  public static method T(k: int, o: A, alt: A, s0: string?): int
    var q: A := o; var s: string? := s0; var t := 0; var i := 0;
    )bzl" + body + R"bzl(
  end T;
  public static method Main(): int
    var bb := new B(); var aa := new A();
    Console.WriteLine($"{T(0, bb, aa, "xy")} {T(0, aa, bb, null)}");
    return 0;
  end Main;
end P;
)bzl");
}

void expectRejected(const JitResult& r, const char* needle) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
}

constexpr const char* kNoMemberY = "no member `y` on type `A`";
constexpr const char* kNullableLength =
    "member access `.Length` on a nullable value requires narrowing it to "
    "non-null first";
constexpr const char* kTypeLostInLoop =
    "`q` does not have its narrowed type here: the type narrowing was "
    "established before the loop";
constexpr const char* kNullLostInLoop =
    "`s` is nullable here: the knowledge that it is non-null was established "
    "before the loop";

}  // namespace

// #6115 through a catch guard.
BEZEL_TEST(sema_catch_when_guard_narrows_6120,
           ref_argument_later_in_the_catch_guard_spec_7_4) {
    auto r = runGuarded(R"bzl(try Th(k); catch var c: Exception when q is B and Rs(ref q, alt) => do return q.y; end, var c2: Exception => do return -1; end end try;
    return -2;)bzl");
    expectRejected(r, kNoMemberY);
}

BEZEL_TEST(sema_catch_when_guard_narrows_6120,
           captured_call_later_in_the_catch_guard_spec_7_4) {
    auto r = runGuarded(R"bzl(var mut := lambda(): bool => do q := alt; return true; end;
    try Th(k); catch var c: Exception when q is B and mut() => do return q.y; end, var c2: Exception => do return -1; end end try;
    return -2;)bzl");
    expectRejected(r, kNoMemberY);
}

// The null twin.
BEZEL_TEST(sema_catch_when_guard_narrows_6120,
           null_ref_argument_later_in_the_catch_guard_spec_7_4) {
    auto r = runGuarded(R"bzl(try Th(k); catch var c: Exception when s <> null and Ns(ref s) => do return s.Length; end, var c2: Exception => do return -1; end end try;
    return -2;)bzl");
    expectRejected(r, kNullableLength);
}

// #6113 through a catch guard.
BEZEL_TEST(sema_catch_when_guard_narrows_6120,
           reassignment_under_an_if_in_the_guarded_catch_body_spec_12_7) {
    auto r = runGuarded(R"bzl(try Th(k); catch var c: Exception when q is B => do if k = 0 then q := alt; end if; return q.y; end, var c2: Exception => do return -1; end end try;
    return -2;)bzl");
    expectRejected(r, kNoMemberY);
}

BEZEL_TEST(sema_catch_when_guard_narrows_6120,
           null_invalidation_under_an_if_in_the_guarded_catch_body_spec_7_4) {
    auto r = runGuarded(R"bzl(try Th(k); catch var c: Exception when s <> null => do if k = 0 then Ns(ref s); end if; return s.Length; end, var c2: Exception => do return -1; end end try;
    return -2;)bzl");
    expectRejected(r, kNullableLength);
}

// #6114 through a catch guard.
BEZEL_TEST(sema_catch_when_guard_narrows_6120,
           loop_in_the_guarded_catch_body_reassigns_below_the_read_spec_12_7) {
    auto r = runGuarded(R"bzl(try Th(k); catch var c: Exception when q is B => do while i < 2 do t := t + q.y; q := alt; i := i + 1; end while; return t; end, var c2: Exception => do return -1; end end try;
    return -2;)bzl");
    expectRejected(r, kTypeLostInLoop);
}

BEZEL_TEST(sema_catch_when_guard_narrows_6120,
           loop_in_the_guarded_catch_body_invalidates_the_null_knowledge_spec_7_4) {
    auto r = runGuarded(R"bzl(try Th(k); catch var c: Exception when s <> null => do while i < 2 do t := t + s.Length; Ns(ref s); i := i + 1; end while; return t; end, var c2: Exception => do return -1; end end try;
    return -2;)bzl");
    expectRejected(r, kNullLostInLoop);
}

// ACCEPT. The `ref` argument precedes the test.
BEZEL_TEST(sema_catch_when_guard_narrows_6120,
           ref_argument_before_the_test_in_the_catch_guard_spec_7_4) {
    auto r = runGuarded(R"bzl(try Th(k); catch var c: Exception when Rs(ref q, o) and q is B => do return q.y; end, var c2: Exception => do return -1; end end try;
    return -2;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 -1\n"));
}

// UNCHANGED PENDING #6141. §7.4 (v0.338) admits the narrowing (the call
// precedes the test), but `q` is mutably captured, so the catch guard keeps
// base behaviour (ArmGuardFacts::seed's #6141 carve-out) and the program is
// rejected exactly as on base. When #6141 lands this row flips back to an
// accept row printing "-1 9".
BEZEL_TEST(sema_catch_when_guard_narrows_6120,
           captured_call_before_the_test_in_the_catch_guard_spec_7_4_unchanged_pending_6141) {
    auto r = runGuarded(R"bzl(var mut := lambda(): bool => do q := alt; return true; end;
    try Th(k); catch var c: Exception when mut() and q is B => do return q.y; end, var c2: Exception => do return -1; end end try;
    return -2;)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "no member `y` on type `A`"));
}

// ACCEPT.
BEZEL_TEST(sema_catch_when_guard_narrows_6120,
           guarded_catch_body_reads_before_it_reassigns_spec_12_7) {
    auto r = runGuarded(R"bzl(try Th(k); catch var c: Exception when q is B => do t := q.y; q := alt; return t; end, var c2: Exception => do return -1; end end try;
    return -2;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 -1\n"));
}

// ACCEPT. A plain null-test guard.
BEZEL_TEST(sema_catch_when_guard_narrows_6120,
           null_test_catch_guard_narrows_spec_7_4) {
    auto r = runGuarded(R"bzl(try Th(k); catch var c: Exception when s <> null => do return s.Length; end, var c2: Exception => do return -1; end end try;
    return -2;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2 -1\n"));
}
