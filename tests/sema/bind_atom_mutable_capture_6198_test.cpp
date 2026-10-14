// fix #6198 (§7.4 "How narrowing is invalidated", §17.15, v0.338): a local
// mutated inside a lambda — assigned, or passed `ref`/`out` — is mutably
// captured, and its narrowing is dropped at a control transfer. The scan that
// collects those names did not descend into a §17.15 binding atom
// `(var x := E; body)`, so
//
//     var mutb := lambda(): bool => (var z := 1; Rs(ref q, alt));
//     if q is B then if mutb() then return q.y; end if; end if;
//
// compiled and read `B.y` off the `A` that `mutb()` had stored (ASan:
// heap-buffer-overflow), where the same lambda written `=> Rs(ref q, alt)` was
// rejected. Every row is on runAot; accept rows assert their output.

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

JitResult runBody(const char* body) {
    return runAot(std::string(R"bzl(module M6198;
public class A
  public constructor() end constructor;
  public x: int := 1;
end A;
public class B: A
  public constructor() end constructor;
  public y: int := 9;
end B;
public class P
  public static method Rs(ref q: A, alt: A): bool
    q := alt;
    return true;
  end Rs;
  public static method Ns(ref s: string?): bool
    s := null;
    return true;
  end Ns;
  public static method T(k: int, o: A, alt: A, s0: string?): int
    var q: A := o; var s: string? := s0;
    )bzl") + body + R"bzl(
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
constexpr const char* kMutablyCaptured =
    "cannot narrow `T?` accessed through a mutably-captured local";

}  // namespace

// THE ISSUE'S REPRO: the lambda's body is a binding atom whose body passes
// the narrowed local `ref`.
BEZEL_TEST(sema_bind_atom_mutable_capture_6198,
           issue_repro_ref_argument_in_a_binding_atom_lambda_body_spec_7_4) {
    auto r = runBody(R"bzl(var mutb := lambda(): bool => (var z := 1; Rs(ref q, alt));
    if q is B then if mutb() then return q.y; end if; end if;
    return -9;)bzl");
    expectRejected(r, kNoMemberY);
}

BEZEL_TEST(sema_bind_atom_mutable_capture_6198,
           ref_argument_in_a_binding_atom_lambda_called_in_the_condition_spec_7_4) {
    auto r = runBody(R"bzl(var mutb := lambda(): bool => (var z := 1; Rs(ref q, alt));
    if q is B and mutb() then return q.y; end if;
    return -9;)bzl");
    expectRejected(r, kNoMemberY);
}

// The mutation is in a CLAUSE of the atom.
BEZEL_TEST(sema_bind_atom_mutable_capture_6198,
           ref_argument_in_a_binding_atom_clause_spec_7_4) {
    auto r = runBody(R"bzl(var mutb := lambda(): bool => (var z := Rs(ref q, alt); z);
    if q is B then if mutb() then return q.y; end if; end if;
    return -9;)bzl");
    expectRejected(r, kNoMemberY);
}

// The LAMBDA is written inside a binding atom.
BEZEL_TEST(sema_bind_atom_mutable_capture_6198,
           assigning_lambda_written_inside_a_binding_atom_spec_7_4) {
    auto r = runBody(R"bzl(var mutb := (var z := 1; lambda(): bool => do q := alt; return true; end);
    if q is B then if mutb() then return q.y; end if; end if;
    return -9;)bzl");
    expectRejected(r, kNoMemberY);
}

// Through a `when` guard (#6082).
BEZEL_TEST(sema_bind_atom_mutable_capture_6198,
           binding_atom_lambda_called_in_a_when_guard_spec_7_4) {
    auto r = runBody(R"bzl(var mutb := lambda(): bool => (var z := 1; Rs(ref q, alt));
    case k of
      _ when q is B and mutb() => do return q.y; end,
      _ => do return -9; end
    end case;
    return -7;)bzl");
    expectRejected(r, kNoMemberY);
}

// The null twin.
BEZEL_TEST(sema_bind_atom_mutable_capture_6198,
           null_ref_argument_in_a_binding_atom_lambda_body_spec_7_4) {
    auto r = runBody(R"bzl(var mutn := lambda(): bool => (var z := 1; Ns(ref s));
    if s <> null then if mutn() then return s.Length; end if; end if;
    return -9;)bzl");
    expectRejected(r, kMutablyCaptured);
}

// ACCEPT. A binding atom in a lambda that does not touch the narrowed local.
BEZEL_TEST(sema_bind_atom_mutable_capture_6198,
           binding_atom_lambda_that_leaves_the_local_alone_spec_7_4) {
    auto r = runBody(R"bzl(var nop := lambda(): bool => (var z := 1; z = 1);
    if q is B then if nop() then return q.y; end if; end if;
    return -9;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 -9\n"));
}

// ACCEPT. The lambda's own parameter `q` shadows the local.
BEZEL_TEST(sema_bind_atom_mutable_capture_6198,
           binding_atom_lambda_mutating_its_own_parameter_spec_7_4) {
    auto r = runBody(R"bzl(var own := lambda(q: A): bool => (var z := 1; Rs(ref q, alt));
    if q is B then if own(alt) then return q.y; end if; end if;
    return -9;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 -9\n"));
}
