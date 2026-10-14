// Spec-derived regression tests for issue #6082 — a `when` guard narrows its
// arm's body.
//
// THE RULE. Spec §7.4 "How narrowing is established" (v0.338): a narrowing
// condition narrows where it is "the condition of an `if` / `elif` / `while`,
// … or a `when` guard (§19.4, narrowing that arm's body)". The narrowing
// conditions are the null tests and "a type test `x is T`"; the same section
// adds that "`not (x is T)` has the same truth value as `x is not T` but
// narrows neither branch".
//
// WHAT WAS BROKEN. The guard of an arm of a `case` WITH a scrutinee was
// type-checked and then ignored: neither `when q is B` nor `when s <> null`
// narrowed anything. MEASURED on 6d8c82eb with the real `bezelc`: the five
// class-/null-carrier accept rows below were rejected (`no member `y` on type
// `A``, the §5.1 base-to-derived error, the §7.4 nullable-member error), and
// the `Object`-carrier row compiled but printed `0 -9 0 -9` — the member read
// went down the permissive path instead of reading `B.y`.
//
// The reject controls pin the three limits the sentence states or implies:
// `not`-composition narrows nothing, the fact is the ARM's (not the next
// arm's), and it does not survive the construct.
//
// Every row is on `runAot`; an accept row compares stdout and takes both the
// guard-true and the guard-false path.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

constexpr const char* kPrelude = R"bzl(module M;
public interface IFoo
  method F(): int;
end IFoo;
public class A: IFoo
  public constructor() end constructor;
  public x: int := 1;
  public method F(): int => x;
end A;
public class B: A
  public constructor() end constructor;
  public y: int := 9;
  public method Y(): int => y;
end B;
public class C: A
  public constructor() end constructor;
  public z: int := 7;
end C;
public enum E
  Red, Green;
end E;
)bzl";

}  // namespace

// THE ISSUE'S SHAPE. `var q: A when q is B` — the guard's `is B` narrows `q` in
// that arm's body, so `q` initialises a `B` and `q.y` reads `B`'s field.
BEZEL_TEST(sema_when_guard_narrows_6082,
           type_test_guard_narrows_class_binding_in_statement_arm_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(o: A): int
    case o of
      var q: A when q is B => do var b: B := q; return b.y + q.y; end,
      _ => do return -9; end
    end case;
  end T;
  public static method Main(): int
    Console.WriteLine($"{T(new B())} {T(new A())}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("18 -9\n"));
}

// The expression form of the same arm.
BEZEL_TEST(sema_when_guard_narrows_6082,
           type_test_guard_narrows_class_binding_in_expression_arm_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(o: A): int => (case o of var q: A when q is B => q.y, _ => -9);
  public static method Main(): int
    Console.WriteLine($"{T(new B())} {T(new A())}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 -9\n"));
}

// The guard tests a variable that is NOT the arm's binding: a constant-pattern
// arm over `k` whose guard narrows the parameter `o` — statement and expression
// forms.
BEZEL_TEST(sema_when_guard_narrows_6082,
           type_test_guard_narrows_outer_class_carrier_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method V(o: A, k: int): int => (case k of 0 when o is B => o.y, _ => -9);
  public static method W(o: A, k: int): int
    case k of
      0 when o is B => do return o.Y(); end,
      _ => do return -9; end
    end case;
  end W;
  public static method Main(): int
    var b := new B();
    var a := new A();
    Console.WriteLine($"{V(b, 0)} {W(b, 0)} {V(a, 0)} {W(a, 0)} {V(b, 1)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 9 -9 -9 -9\n"));
}

// An `Object` carrier: the guard narrows it to `B`, so the member read and the
// `B`-typed initializer are typed against `B`.
BEZEL_TEST(sema_when_guard_narrows_6082,
           type_test_guard_narrows_object_carrier_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(o: Object): int
    case o of
      var q: Object when o is B => do var b: B := o; return b.y + o.y; end,
      _ => do return -9; end
    end case;
  end T;
  public static method U(o: Object, k: int): int => (case k of 0 when o is B => o.Y(), _ => -9);
  public static method Main(): int
    var b: Object := new B();
    var a: Object := new A();
    Console.WriteLine($"{T(b)} {T(a)} {U(b, 0)} {U(a, 0)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("18 -9 9 -9\n"));
}

// The same §7.4 sentence covers a null test: `when s <> null` narrows `s` to
// non-null in that arm's body.
BEZEL_TEST(sema_when_guard_narrows_6082,
           null_test_guard_narrows_nullable_carrier_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(s: string?, k: int): int
    case k of
      var q: int when s <> null => do return s.Length; end,
      _ => do return -9; end
    end case;
  end T;
  public static method U(s: string?, k: int): int => (case k of 0 when s <> null => s.Length, _ => -9);
  public static method Main(): int
    Console.WriteLine($"{T("abc", 1)} {U("ab", 0)} {T(null, 1)} {U(null, 0)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3 2 -9 -9\n"));
}

// REJECT CONTROL. §7.4: "`not (x is T)` has the same truth value as `x is not
// T` but narrows neither branch" — and the guard's TRUE branch is the one where
// `o` is NOT a `B` anyway.
BEZEL_TEST(sema_when_guard_narrows_6082,
           not_composed_type_test_guard_narrows_nothing_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(o: A, k: int): int => (case k of 0 when not (o is B) => o.y, _ => -9);
  public static method Main(): int
    Console.WriteLine($"{T(new B(), 0)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `y` on type `A`"));
}

// REJECT CONTROL. "narrowing that arm's body": the arm AFTER the guarded one
// sees `o` at its declared type.
BEZEL_TEST(sema_when_guard_narrows_6082,
           guard_narrowing_does_not_reach_the_next_arm_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(o: A, k: int): int => (case k of 0 when o is B => o.y, _ => o.y);
  public static method Main(): int
    Console.WriteLine($"{T(new B(), 0)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `y` on type `A`"));
}

// REJECT CONTROL. Nor does the code after the construct: the guarded arm falls
// through, and `o.y` after `end case` is a member read on an `A`.
BEZEL_TEST(sema_when_guard_narrows_6082,
           guard_narrowing_does_not_survive_the_case_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(o: A, k: int): int
    var n := 0;
    case k of
      0 when o is B => do n := o.y; end,
      _ => do n := -9; end
    end case;
    return n + o.y;
  end T;
  public static method Main(): int
    Console.WriteLine($"{T(new B(), 0)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `y` on type `A`"));
}

// REJECT CONTROL for the null fact: after the construct `s` is `string?` again.
BEZEL_TEST(sema_when_guard_narrows_6082,
           null_guard_narrowing_does_not_survive_the_case_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(s: string?, k: int): int
    var n := 0;
    case k of
      0 when s <> null => do n := s.Length; end,
      _ => do n := -9; end
    end case;
    return n + s.Length;
  end T;
  public static method Main(): int
    Console.WriteLine($"{T("abc", 0)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "on a nullable value requires narrowing it to non-null first (§7.4)"));
}

// An enclosing `if o is A` narrowing that a guard refines to `B` is back in
// force after the construct — the guard's fact ends with its arm and does not
// take the outer one with it.
BEZEL_TEST(sema_when_guard_narrows_6082,
           guard_keeps_the_enclosing_narrowing_after_the_case_spec_7_4) {
    auto r = runAot(std::string(kPrelude) + R"bzl(public class P
  public static method T(o: Object, k: int): int
    if o is A then
      var n := 0;
      case k of
        0 when o is B => do n := o.y; end,
        _ => do n := 100; end
      end case;
      return n + o.x;
    end if;
    return -1;
  end T;
  public static method Main(): int
    var b: Object := new B();
    var a: Object := new A();
    Console.WriteLine($"{T(b, 0)} {T(a, 0)} {T(b, 1)}");
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("10 101 101\n"));
}

// ---------------------------------------------------------------------------
// A guard-narrowed body is invalidated like an `if`-narrowed one (§7.4 "How
// narrowing is invalidated", §12.7, v0.338). These rows pin the shapes of
// #6113 / #6114 / #6115 REACHED THROUGH A GUARD — each compiled on the first
// version of this fix and read a `B` field off an `A` (or dereferenced null)
// — and the accept rows that sit next to them. They share one driver.

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
  public static method T(k: int, o: A, alt: A, s0: string?, an: A?): int
    var s: string? := s0; var t := 0; var i := 0;
    )bzl" + body + R"bzl(
  end T;
  public static method Main(): int
    var bb := new B(); var aa := new A();
    Console.WriteLine($"{T(0, bb, aa, "xy", bb)} {T(0, aa, bb, null, null)}");
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

// #6115 through a guard: a `ref` argument after the test, in the same guard,
// invalidates the narrowing (§7.4 "How narrowing is invalidated").
BEZEL_TEST(sema_when_guard_narrows_6082,
           ref_argument_later_in_the_guard_statement_arm_spec_7_4) {
    auto r = runGuarded(R"bzl(case o of
      var q: A when q is B and Rs(ref q, alt) => do return q.y; end,
      _ => do return -9; end
    end case;
    return -7;)bzl");
    expectRejected(r, kNoMemberY);
}

BEZEL_TEST(sema_when_guard_narrows_6082,
           ref_argument_later_in_the_guard_expression_arm_spec_7_4) {
    auto r = runGuarded(R"bzl(return (case o of var q: A when q is B and Rs(ref q, alt) => q.y, _ => -9);)bzl");
    expectRejected(r, kNoMemberY);
}

// …and a call that can reach a closure assigning the tested local.
BEZEL_TEST(sema_when_guard_narrows_6082,
           captured_call_later_in_the_guard_spec_7_4) {
    auto r = runGuarded(R"bzl(var q: A := o; var mut := lambda(): bool => do q := alt; return true; end;
    case k of
      _ when q is B and mut() => do return q.y; end,
      _ => do return -9; end
    end case;
    return -7;)bzl");
    expectRejected(r, kNoMemberY);
}

// The null twin.
BEZEL_TEST(sema_when_guard_narrows_6082,
           null_ref_argument_later_in_the_guard_statement_arm_spec_7_4) {
    auto r = runGuarded(R"bzl(case k of
      _ when s <> null and Ns(ref s) => do return s.Length; end,
      _ => do return -9; end
    end case;
    return -7;)bzl");
    expectRejected(r, kNullableLength);
}

BEZEL_TEST(sema_when_guard_narrows_6082,
           null_ref_argument_later_in_the_guard_expression_arm_spec_7_4) {
    auto r = runGuarded(R"bzl(return (case k of _ when s <> null and Ns(ref s) => s.Length, _ => -9);)bzl");
    expectRejected(r, kNullableLength);
}

// #6113 through a guard: the body reassigns under an else-less `if`.
BEZEL_TEST(sema_when_guard_narrows_6082,
           reassignment_under_an_if_in_the_guarded_body_spec_12_7) {
    auto r = runGuarded(R"bzl(case o of
      var q: A when q is B => do if k = 0 then q := alt; end if; return q.y; end,
      _ => do return -9; end
    end case;
    return -7;)bzl");
    expectRejected(r, kNoMemberY);
}

BEZEL_TEST(sema_when_guard_narrows_6082,
           null_invalidation_under_an_if_in_the_guarded_body_spec_7_4) {
    auto r = runGuarded(R"bzl(case k of
      _ when s <> null => do if k = 0 then Ns(ref s); end if; return s.Length; end,
      _ => do return -9; end
    end case;
    return -7;)bzl");
    expectRejected(r, kNullableLength);
}

// #6114 through a guard: a loop in the body reads through the guard's narrowing
// and then reassigns.
BEZEL_TEST(sema_when_guard_narrows_6082,
           loop_in_the_guarded_body_reassigns_below_the_read_spec_12_7) {
    auto r = runGuarded(R"bzl(case o of
      var q: A when q is B => do while i < 2 do t := t + q.y; q := alt; i := i + 1; end while; return t; end,
      _ => do return -9; end
    end case;
    return -7;)bzl");
    expectRejected(r, kTypeLostInLoop);
}

BEZEL_TEST(sema_when_guard_narrows_6082,
           loop_in_the_guarded_body_invalidates_the_null_knowledge_spec_7_4) {
    auto r = runGuarded(R"bzl(case k of
      _ when s <> null => do while i < 2 do t := t + s.Length; Ns(ref s); i := i + 1; end while; return t; end,
      _ => do return -9; end
    end case;
    return -7;)bzl");
    expectRejected(r, kNullLostInLoop);
}

// ACCEPT. The `ref` argument comes BEFORE the test: the test is of the value
// the callee left.
BEZEL_TEST(sema_when_guard_narrows_6082,
           ref_argument_before_the_test_in_the_guard_spec_7_4) {
    auto r = runGuarded(R"bzl(case o of
      var q: A when Rs(ref q, o) and q is B => do return q.y; end,
      _ => do return -9; end
    end case;
    return -7;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 -9\n"));
}

// ACCEPT.
BEZEL_TEST(sema_when_guard_narrows_6082,
           captured_call_before_the_test_in_the_guard_spec_7_4) {
    auto r = runGuarded(R"bzl(var q: A := o; var mut := lambda(): bool => do q := alt; return true; end;
    case k of
      _ when mut() and q is B => do return q.y; end,
      _ => do return -9; end
    end case;
    return -7;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("-9 9\n"));
}

// ACCEPT. The read precedes the reassignment.
BEZEL_TEST(sema_when_guard_narrows_6082,
           guarded_body_reads_before_it_reassigns_spec_12_7) {
    auto r = runGuarded(R"bzl(case o of
      var q: A when q is B => do t := q.y; q := alt; return t; end,
      _ => do return -9; end
    end case;
    return -7;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 -9\n"));
}

// #6155's repro shape: the guard's null test of the SCRUTINEE narrows it in the
// arm body.
BEZEL_TEST(sema_when_guard_narrows_6082,
           null_test_guard_narrows_the_scrutinee_statement_arm_spec_7_4_issue_6155) {
    auto r = runGuarded(R"bzl(case an of
      var q when an <> null => do return an.x; end,
      var u => do return -1; end
    end case;
    return -7;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1 -1\n"));
}

BEZEL_TEST(sema_when_guard_narrows_6082,
           null_test_guard_narrows_the_scrutinee_expression_arm_spec_7_4_issue_6155) {
    auto r = runGuarded(R"bzl(return (case an of var q when an <> null => an.x, var u => -1);)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1 -1\n"));
}
