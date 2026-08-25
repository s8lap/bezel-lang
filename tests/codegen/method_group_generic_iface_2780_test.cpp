// Regression pin for issue #2780 — the SECOND defect at makeMethodThunk's
// signature site that #2766 left behind. #2766 substituted the receiver's
// generic argument for the parameter type on TWO stdlib-stated single-
// parameter signatures (`IEquatable<T>.Equals` / `IComparable<T>.CompareTo`),
// reasoning the mapping from the document rather than the symbol table.
//
// The general defect remains on every OTHER generic interface method reached
// as a method group. The mechanism is identical: an interface method has no
// concrete origFn, so `makeMethodThunk` derives its LLVM signature from
// `methodFnType(ms)` — and the synthetic builtin interface methods record no
// resolved declared type for their parameters (`ps->resolvedDeclType == null`),
// so `methodFnType` lowers the parameter to the placeholder `i32` for EVERY
// instantiation. The thunk therefore advertises `i32 (ptr env, i32)` where
// both the call site and the itable target use `ptr`. On the plain oracle a
// short string literal survives (small-program malloc fits in 32 bits); under
// ASan the allocator hands out high addresses and the callee SEGVs on the
// truncated pointer.
//
// #2766's scoping comment specifically called this out as the residue the
// principled fix should cover: "Every OTHER generic interface method reached
// as a method group, when the type argument is reference-shaped. Unprobed
// individually; the mechanism is identical and proven above."
//
// THIS TEST generalises the substitution: each parameter (and the return
// type) whose DECLARED type is a bare type-parameter reference matching
// `ms->parent->genericParams[i]->name` is substituted with
// `recvStaticTy->genericArgs[i]`. Skips by-ref parameters (§22.3 — those
// are already pointer-to-slot). Two evidence sources for the declared type:
// the parameter symbol's `resolvedDeclType` (imported methods, set by
// ModuleLoader) and the AST node's `declType->name->parts[0]` (synthetic
// builtin interface methods, whose `resolvedDeclType` is null by design —
// the same null that originally caused `methodFnType`'s i32 fallback).
//
// Every test here fails-without the fix and passes-with it under ASan. On the
// plain oracle the value happens to come out right because the small test
// process's malloc returns < 4 GiB, so the ASan oracle is the one that pins
// the defect: a SEGV inside the callee at the truncated pointer.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

bool wasSkipped(const JitResult& r) {
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.message.find("SKIP") != std::string::npos) return true;
    }
    return false;
}

int runPlain(const std::string& src) {
    JitResult r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

int runChecked(const std::string& src) {
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return -1000;
    return r.ran ? r.exitCode : -999;
}

// ASan on, LeakSanitizer off — see #2766 test's runCheckedNoLeaks rationale;
// the pre-existing string-temporary leaks (#2717/#2764 family) leak identically
// against the pre-fix compiler, and LSan overrides the process exit code.
int runCheckedNoLeaks(const std::string& src) {
    JitResult r = runAotCheckedLenient(src);
    if (wasSkipped(r)) return -1000;
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// =============================================================================
// USER-DEFINED GENERIC INTERFACE METHODS — the original #2780 scope.
// =============================================================================

// §27.6 / §14.1 — a user-defined generic interface with a REFERENCE-typed
// parameter reached as a method group. The thunk must advertise the
// receiver's instantiated argument (`string` → ptr), not the interface's
// declared `T` placeholder (`i32`). Before the fix, the thunk truncated the
// heap-string argument to 32 bits and the callee SEGVd inside the heap
// dereference (the heap string built by concatenation lives at a high
// address under ASan).
// Spec citation: §27.6 (method-group), §14.1 (generic substitution).
BEZEL_TEST(method_group_generic_iface_2780, user_iface_param_string_no_truncation) {
    BEZEL_EXPECT_EQ(runCheckedNoLeaks(
        "module M; "
        "interface IConsumer<T> "
        "  method Consume(x: T): int; "
        "end IConsumer; "
        "class StrCons: IConsumer<string> "
        "  method Consume(x: string): int => x.Length; "
        "end StrCons; "
        "class C public static method Main(): int "
        "  var c: IConsumer<string> := new StrCons(); "
        "  var f := c.Consume; "
        "  var pre := \"abcdefghijklmnopqrstuvwxyz-\"; "
        "  var post := \"ABCDEFGHIJKLMNOPQRSTUVWXYZ\"; "
        "  var s := pre + post + pre + post + pre + post; "
        "  return f(s); "
        "end Main; end C;"), 159);
}

// §27.6 / §14.1 — the same shape with an `int`-typed parameter (the
// reference-vs-value side of the substitution). The thunk must take `i32`
// (not `ptr`) for the value-shaped instantiation; the substitution's lower
// path produces the same type, so the value result must be correct.
BEZEL_TEST(method_group_generic_iface_2780, user_iface_param_int_passes_through) {
    BEZEL_EXPECT_EQ(runPlain(
        "module M; "
        "interface IConsumer<T> "
        "  method Consume(x: T): int; "
        "end IConsumer; "
        "class IntCons: IConsumer<int> "
        "  method Consume(x: int): int => x * 2; "
        "end IntCons; "
        "class C public static method Main(): int "
        "  var c: IConsumer<int> := new IntCons(); "
        "  var f := c.Consume; "
        "  return f(21); "
        "end Main; end C;"), 42);
}

// §27.6 / §14.1 — a USER-DEFINED generic interface with TWO type parameters
// (the positional-mapping concern #2766 deferred because it would have been a
// guess). The fix must substitute parameter 0 with genericArgs[0] and
// parameter 1 with genericArgs[1], independently — exactly the
// name→genericArgs-index mapping the parameter's declared type's
// `headName()` provides. Pre-fix this thunk truncated the string argument to
// 32 bits; the SEGV is in the callee at the dereference of the truncated
// pointer.
BEZEL_TEST(method_group_generic_iface_2780, user_iface_two_type_params_correctly_mapped) {
    BEZEL_EXPECT_EQ(runCheckedNoLeaks(
        "module M; "
        "interface IPair<K, V> "
        "  method Combine(k: K, v: V): int; "
        "end IPair; "
        "class KVCombine: IPair<string, int> "
        "  method Combine(k: string, v: int): int => k.Length + v; "
        "end KVCombine; "
        "class C public static method Main(): int "
        "  var p: IPair<string, int> := new KVCombine(); "
        "  var f := p.Combine; "
        "  var a := \"hello\"; "
        "  var b := a + \"-world\"; "
        "  return f(b, 100); "
        "end Main; end C;"), 111);
}

// =============================================================================
// BUILT-IN INTERFACE METHODS NOT IN #2766's HARDCODED LIST.
// =============================================================================

// §27.6 / §14.1 — `IEqualityComparer<T>.Equals(a: T, b: T)`. The builtin
// interface carries the same `ParameterSymbol::resolvedDeclType == null` that
// caused `methodFnType`'s i32 fallback. Pre-fix the thunk advertises
// `i1 (ptr env, i32, i32)`; the callee is `bz$M.StringEq$Equals(ptr, ptr,
// ptr)` and dereferences the truncated pointer under ASan.
BEZEL_TEST(method_group_generic_iface_2780, iequality_comparer_equals_string_no_truncation) {
    BEZEL_EXPECT_EQ(runCheckedNoLeaks(
        "module M; "
        "class StringEq: IEqualityComparer<string> "
        "  method Equals(a: string, b: string): bool => a = b; "
        "  method GetHashCode(value: string): int => value.Length; "
        "end StringEq; "
        "class C public static method Main(): int "
        "  var ec: IEqualityComparer<string> := new StringEq(); "
        "  var f := ec.Equals; "
        "  var a := \"hi\"; "
        "  var b := \"h\" + \"i\"; "
        "  if f(a, b) then return 5; end; "
        "  return 7; "
        "end Main; end C;"), 5);
}

// §27.6 / §14.1 — `IEqualityComparer<T>.GetHashCode(value: T)`. Same shape,
// single string parameter; pre-fix the parameter is i32 (placeholder) and
// the callee dereferences a heap-string address truncated to 32 bits.
BEZEL_TEST(method_group_generic_iface_2780, iequality_comparer_gethashcode_string_no_truncation) {
    BEZEL_EXPECT_EQ(runCheckedNoLeaks(
        "module M; "
        "class StringEq: IEqualityComparer<string> "
        "  method Equals(a: string, b: string): bool => a = b; "
        "  method GetHashCode(value: string): int => value.Length; "
        "end StringEq; "
        "class C public static method Main(): int "
        "  var ec: IEqualityComparer<string> := new StringEq(); "
        "  var f := ec.GetHashCode; "
        "  var a := \"abcdefghijklmnop\"; "
        "  var b := a + \"qrstuvwxyz\"; "
        "  return f(b); "
        "end Main; end C;"), 26);
}

// =============================================================================
// CONTROLS — non-generic interfaces and concrete-class receivers.
// =============================================================================

// §27.6 — CONTROL (passes before AND after). A NON-generic interface with a
// string parameter must keep dispatching through the itable to the
// implementing class. The generalisation gate (`recvStaticTy->declaration ==
// ms->parent` AND `genericArgs.size() == genericParams.size()`) is satisfied
// for the receiver's static type, but every parameter has a CONCRETE
// declared type (not a bare type-param reference), so the predicate does not
// match and the substitution is a no-op.
BEZEL_TEST(method_group_generic_iface_2780, non_generic_iface_method_group_unchanged) {
    BEZEL_EXPECT_EQ(runCheckedNoLeaks(
        "module M; "
        "interface IGreeter "
        "  method Greet(name: string): int; "
        "end IGreeter; "
        "class Greeter: IGreeter "
        "  method Greet(name: string): int => name.Length; "
        "end Greeter; "
        "class C public static method Main(): int "
        "  using var g := new Greeter(); "
        "  var ig: IGreeter := g; "
        "  var f := ig.Greet; "
        "  var s := \"hello\"; "
        "  var t := s + \"-world\"; "
        "  return f(t); "
        "end Main; end C;"), 11);
}

// §27.6 — CONTROL (passes before AND after). A user-defined class declaring
// the interface, reached as a method group through the CLASS receiver (not
// the interface). `recvStaticTy->declaration == ms->parent` is false (the
// receiver's static type is the class, not the interface), so the gate does
// not fire — the thunk dispatches through the concrete vtable to the class's
// own Equals, which has the real signature. The fix must not disturb this
// path.
BEZEL_TEST(method_group_generic_iface_2780, concrete_class_receiver_dispatches_to_origfn) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class SC: IEquatable<string> "
        "  public method Equals(other: string): bool => other = \"2780-ctrl\"; "
        "end SC; "
        "class C public static method Main(): int "
        "  using var sc := new SC(); "
        "  var f := sc.Equals; "
        "  if f(\"2780-ctrl\") then return 13; end; "
        "  return 3; "
        "end Main; end C;"), 13);
}
