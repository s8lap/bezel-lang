// Spec-derived regression test for issue #2186 — the §12.5/§11.2 interface-
// conformance check satisfied an inherited abstract/interface member by NAME
// (+arity) ONLY; it never compared the member's SUBSTITUTED signature against
// the implementing member for the specific interface instantiation being
// declared. So a class could declare a SECOND instantiation of a generic
// interface and have it "implemented" by a base-class member of the WRONG
// instantiation:
//
//   class K<T> : IM<T>                        // K<T>::Get(): T?
//     public method Get(): T? ...
//   class D : K<string>, IM<int>              // accepted pre-fix; IM<int> has
//                                              // no Get(): int? impl
//
// Per §11.2 (matching rule, v0.220): "A member matches an interface member
// when it ... has the same name, the same arity, the same parameter modifiers
// (`ref`, `out`, `owned`, `scoped`), parameter types that are exactly equal
// UNDER THE INTERFACE'S SUBSTITUTION, and a return type that is exactly equal
// UNDER THE INTERFACE'S SUBSTITUTION." §12.5 requires every concrete subclass
// to implement each inherited abstract/interface member — under that
// substituted-signature rule. Pre-#2186 the check compared name+arity+modifier
// only, leaving the substituted type comparison (#1685's own comment called it
// out as "a follow-up") unimplemented.
//
// The AOT run oracle is used so the diagnostic fires through the full
// front-end + AOT path: REJECT tests `!ran && diagnostics.hasErrors()` (and
// the §11.2 message is named); ACCEPT controls `ran && exitCode == expected`.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Compile-and-run via the AOT pipeline; returns true iff the program compiled,
// linked, and ran to normal termination with the expected exit status.
bool runs(int expected, const std::string& src) {
    auto r = runAot(src);
    return r.ran && r.exitCode == expected;
}

// Compile-and-run via the AOT pipeline; returns true iff the program was
// REJECTED at compile time with at least one error diagnostic.
bool rejects(const std::string& src) {
    auto r = runAot(src);
    return !r.ran && r.diagnostics.hasErrors();
}

// True iff an ERROR diagnostic mentions the #2186 substituted-signature
// conformance reason — pins THIS defect (its message names "interface" and
// "substitution" and cites §11.2, the substrings the name-only checks do NOT
// carry).
bool hasSubstConformanceError(const std::string& src) {
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("11.2") == std::string::npos) continue;
        if (d.message.find("interface") == std::string::npos) continue;
        if (d.message.find("substitution") == std::string::npos) continue;
        return true;
    }
    return false;
}

// ---- REJECT case 1 (the issue's repro, return-type mismatch) --------------
// D : K<string>, IM<int>. K<string>'s inherited Get(): string? cannot satisfy
// IM<int>::Get(): int? — the substituted signatures (int? vs string?) differ.
// FAIL-WITHOUT: compiles clean and runs (returning 7 — the wrong-typed Get()
// dispatches against a null and reads 7); PASS-WITH: rejected at the
// conformance site with the §11.2 substituted-signature diagnostic.
const char* kWrongInstantiationReturnRejected =
    "module M; "
    "public interface IM<T> method Get(): T?; end IM; "
    "class K<T> : IM<T> public method Get(): T? return null; end Get; end K; "
    "class D : K<string>, IM<int> end D; "
    "public class C "
    "  public static method Main(): int "
    "    using var d := new D(); "
    "    var s: IM<int> := d; "
    "    var v: int? := s.Get(); "
    "    return (if v = null then 7 else 8); "
    "  end Main; "
    "end C;";

// ---- REJECT case 2 (parameter-type mismatch, same shape) ------------------
// IM<T>.Push(x: T); D : K<string>, IM<int>. K<string>::Push(x: string) cannot
// satisfy IM<int>::Push(x: int) — parameter types differ under substitution.
const char* kWrongInstantiationParamRejected =
    "module M; "
    "public interface IM<T> method Push(x: T): int; end IM; "
    "class K<T> : IM<T> "
    "  public method Push(x: T): int return 0; end Push; "
    "end K; "
    "class D : K<string>, IM<int> end D; "
    "public class C "
    "  public static method Main(): int "
    "    using var d := new D(); "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---- ACCEPT control 1 (single inherited instantiation) --------------------
// D : K<string> with K<T> : IM<T>. The only IM obligation at D is IM<string>
// (inherited from K<string>); K<string>::Get(): string? satisfies it under
// the matching substitution. Must STAY accepted (no over-rejection of the
// legitimate single-instantiation case).
const char* kSingleInstantiationInheritedAccepted =
    "module M; "
    "public interface IM<T> method Get(): T?; end IM; "
    "class K<T> : IM<T> public method Get(): T? return null; end Get; end K; "
    "class D : K<string> end D; "
    "public class C "
    "  public static method Main(): int "
    "    using var d := new D(); "
    "    var s: IM<string> := d; "
    "    var v: string? := s.Get(); "
    "    return (if v = null then 7 else 8); "
    "  end Main; "
    "end C;";

// ---- ACCEPT control 2 (direct non-generic conformance) -------------------
// C : IM<int> provides Get(): int? directly. The simplest conformance; the
// substituted signature (int?) trivially matches the impl's (int?). Must
// STAY accepted.
const char* kDirectNonGenericAccepted =
    "module M; "
    "public interface IM<T> method Get(): T?; end IM; "
    "public class C : IM<int> "
    "  public method Get(): int? return null; end Get; "
    "  public static method Main(): int "
    "    using var c := new C(); "
    "    var s: IM<int> := c; "
    "    var v: int? := s.Get(); "
    "    return (if v = null then 7 else 8); "
    "  end Main; "
    "end C;";

// ---- ACCEPT control 3 (direct generic conformance, T → T) ----------------
// C<T> : IM<T> provides Get(): T?. Both sides substitute T → T (identity) and
// match. Must STAY accepted — the generic-class direct-conformance case.
const char* kDirectGenericIdentityAccepted =
    "module M; "
    "public interface IM<T> method Get(): T?; end IM; "
    "public class C<T> : IM<T> "
    "  public method Get(): T? return null; end Get; "
    "  public static method Main(): int "
    "    using var c := new C<int>(); "
    "    var s: IM<int> := c; "
    "    var v: int? := s.Get(); "
    "    return (if v = null then 7 else 8); "
    "  end Main; "
    "end C;";

// ---- ACCEPT control 4 (§11.2 T → T? parameter-widening admitted) ---------
// Interface declares M(x: T); implementation declares M(x: T?). §11.2 admits
// this widening (the implementation accepts more inputs than the interface
// promises to pass). Must STAY accepted — the widening the fix preserves.
const char* kParamNullabilityWideningAccepted =
    "module M; "
    "public interface IM<T> method M(x: T): int; end IM; "
    "public class C : IM<string> "
    "  public method M(x: string?): int return 0; end M; "
    "  public static method Main(): int "
    "    using var c := new C(); "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// ---- FAIL-without / PASS-with: the wrong-instantiation inherited member is
//      REJECTED with the §11.2 substituted-signature diagnostic. ------------

// (1) The issue's repro: return-type mismatch. Pre-#2186 this compiles and
// runs (exit 7 — wrong-typed dispatch against a null); post-fix it is rejected
// with the §11.2 diagnostic naming the substituted-signature conformance rule.
BEZEL_TEST(iface_conformance_subst_signature_2186,
           wrong_instantiation_return_type_rejected_11_2) {
    BEZEL_EXPECT_TRUE(rejects(kWrongInstantiationReturnRejected));
    BEZEL_EXPECT_TRUE(
        hasSubstConformanceError(kWrongInstantiationReturnRejected));
}

// (2) Same shape, parameter-type mismatch. Same fail-without / pass-with.
BEZEL_TEST(iface_conformance_subst_signature_2186,
           wrong_instantiation_param_type_rejected_11_2) {
    BEZEL_EXPECT_TRUE(rejects(kWrongInstantiationParamRejected));
    BEZEL_EXPECT_TRUE(
        hasSubstConformanceError(kWrongInstantiationParamRejected));
}

// ---- ACCEPT controls (over-rejection safety — these must NOT regress) -----

// (3) Single inherited instantiation (no second `: IM<int>` declaration) is
// the legitimate shape and stays accepted.
BEZEL_TEST(iface_conformance_subst_signature_2186,
           single_instantiation_inherited_accepted) {
    BEZEL_EXPECT_TRUE(runs(7, kSingleInstantiationInheritedAccepted));
}

// (4) Direct non-generic conformance stays accepted.
BEZEL_TEST(iface_conformance_subst_signature_2186,
           direct_non_generic_accepted) {
    BEZEL_EXPECT_TRUE(runs(7, kDirectNonGenericAccepted));
}

// (5) Direct generic conformance with T → T identity substitution stays
// accepted.
BEZEL_TEST(iface_conformance_subst_signature_2186,
           direct_generic_identity_accepted) {
    BEZEL_EXPECT_TRUE(runs(7, kDirectGenericIdentityAccepted));
}

// (6) The §11.2 T → T? parameter nullability widening stays accepted — the
// fix admits exactly this widening and no more.
BEZEL_TEST(iface_conformance_subst_signature_2186,
           param_nullability_widening_accepted_11_2) {
    BEZEL_EXPECT_TRUE(runs(0, kParamNullabilityWideningAccepted));
}
