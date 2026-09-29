// Regression tests for issue #4947 — §11.2 ("Interface declaration": a member
// matches an interface member with "parameter types that are exactly equal
// under the interface's substitution") / §14.1 ("Generic declarations":
// `ctx_.exprType` on a use site carries the post-substitution sema type) /
// §23 (Properties and Indexers), v0.307.
//
// THE DEFECT. `lowerVirtualPropertySet` (the itable/vtable dispatch for
// `obj.P := v`) built the setter call's parameter type from the bare AST
// declaration (`lowerType(pd->type.get())`) UNCONDITIONALLY, ignoring the
// sema-resolved, substituted type of the assignment target. For a generic
// interface property (`property Q: T;` on `I<T>`), lowering the bare `T` with
// no active genericSubst_ at the call site resolves to the i32 ERASED
// PLACEHOLDER, while the per-instantiation itable/vtable slot the call
// dispatches through expects the SPECIALIZED type (e.g. `ptr` for `T =
// string`). The mismatch aborted the build:
//
//     Call parameter type does not match function signature!
//     ptr @.bz.str.55
//      i32  call void %pset(ptr %22, ptr @.bz.str.55)
//     bezelc: codegen error: IR verification failed
//
// on the issue's own repro:
//
//     public interface I<T> property Q: T; end I;
//     public class B<T>: I<T>
//       public constructor(v: T) Q := v; end constructor;
//       public property Q: T;
//     end B;
//     ... using var e := new B<string>("e"); var s: I<string> := e;
//     s.Q := "q";
//
// and — per the issue's own note — identically for a non-generic derived
// receiver (`class E: B<string>`).
//
// THE FIX. `lowerVirtualPropertySet` now computes `valTy` from
// `ctx_.exprType(target)` FIRST — the identical sema type the very same
// function already reads two statements later for `boxForTarget(v,
// ctx_.exprType(target), valueExpr)` — falling back to `pd->type.get()` (or
// the imported-property side table) only when sema has no static type for the
// target. This mirrors the getter's #2944 fix exactly.
//
// SHAPE CHOSEN FOR THE ROUND-TRIP ASSERTIONS. Every row below uses the
// non-generic-derived-receiver shape (`class E: B<string>` / `E: B<Elem>`),
// NOT a bare direct instantiation (`new B<string>(...)` used without a
// wrapping non-generic class). MEASURED while writing this test: on a bare
// direct instantiation, `specializedVtableFor`'s itable-slot patch
// (LlvmBackend_Setup.cpp, `patchSlot`'s
// `specFn->getFunctionType() != canon->getFunctionType()` guard) refuses to
// install the specialized (`ptr`) accessor into the property's itable
// GET/SET slots whenever the specialized signature differs from the erased
// i32 template's — which is exactly the case a reference-typed `T` produces
// — so the slot is left pointing at the i32-erased template accessor. The
// call site built by THIS fix is correctly typed, but the callee it reaches
// still has the OLD i32 ABI, so reads/writes silently land at the wrong
// width with no diagnostic: `s.Q` read empty immediately after construction
// (no set involved at all), and a write through the interface did not
// reach the same storage the concrete-typed getter reads. That is a
// SEPARATE, PRE-EXISTING defect from this one (it also affects the plain
// GETTER, which #2944 already fixed at the call site, with no compile-time
// symptom at all — just silently wrong values) and is filed on its own
// (see the handback for the issue number) rather than fixed here, per scope
// discipline. The `E: B<T>` shape is unaffected: it is specialized by
// `patchConcreteDerivedGenericBaseItables` (#4933/#4966), a different code
// path with no such guard, so the round trip below is real end-to-end
// evidence of THIS fix rather than resting on the other, separately-filed
// defect.
//
// Coverage: T = string (the issue's own repro shape), T = a class/reference
// type (Elem, exercising a `ptr` receiver-object parameter rather than a
// boxed string handle), and T = int (a primitive control, on a bare direct
// instantiation — the erased i32 placeholder already happens to equal the
// substituted type here, so no itable specialization is even needed and this
// case must stay unchanged by the fix). Every row asserts exact stdout on
// the plain AOT path and again under generated-code ASan (runAotChecked).

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

void expectBoth(const char* src, const std::string& expected) {
    JitResult plain = runAot(src);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    BEZEL_EXPECT_EQ(plain.stdoutText, expected);
    JitResult checked = runAotChecked(src);
    BEZEL_EXPECT_TRUE(checked.ran);
    BEZEL_EXPECT_EQ(checked.exitCode, 0);
    BEZEL_EXPECT_EQ(checked.stdoutText, expected);
}

}  // namespace

// ─── T1 — T = string, on the issue's own "non-generic E: B<string>" variant ─
// Pre-fix: does not compile at all (IR verification abort on the setter
// call, MEASURED — see the header comment). Post-fix: the write through the
// interface lands, visible both through the interface view and the concrete
// receiver.
BEZEL_TEST(interface_generic_setter_param_type_4947, string_t_nongeneric_derived) {
    expectBoth(R"BZL(
module M;
public interface I<T>
  property Q: T;
end I;
public class B<T>: I<T>
  public constructor(v: T) Q := v; end constructor;
  public property Q: T;
end B;
public class E: B<string>
  public constructor() base("e"); end constructor;
end E;
public class Program
  public static method Main(): int
    using var e := new E();
    var s: I<string> := e;
    Console.WriteLine(s.Q);
    s.Q := "q";
    Console.WriteLine(s.Q);
    Console.WriteLine(e.Q);
    return 0;
  end Main;
end Program;
)BZL",
               "e\nq\nq\n");
}

// ─── T2 — a second reference type: a class, not just `string` ─────────────
// Exercises the same substituted-parameter-type rule for a plain heap-object
// pointer rather than a boxed string handle, again on the `E: B<Elem>`
// shape. The replacement value is passed through a helper method's parameter
// (`SetQ`), not assigned from a `using var` local directly in `Main` — a
// PARAMETER-sourced borrow is admissible into the non-owned property `Q`
// (§28.3's borrowed-field store rule), while a scope-bound LOCAL is rejected
// at compile time ("borrow ... stored into a field would outlive its
// source"), MEASURED while writing this test. Pre-fix: does not compile
// (the same IR verification abort). Post-fix: the reassigned Elem is visible
// through both views.
BEZEL_TEST(interface_generic_setter_param_type_4947, class_reference_t_nongeneric_derived) {
    expectBoth(R"BZL(
module M;
public interface I<T>
  property Q: T;
end I;
public class B<T>: I<T>
  public constructor(v: T) Q := v; end constructor;
  public property Q: T;
end B;
public class Elem
  public V: int;
  public constructor(v: int) V := v; end constructor;
end Elem;
public class E: B<Elem>
  public constructor(v: Elem) base(v); end constructor;
end E;
public class Program
  public static method SetQ(target: I<Elem>, v: Elem)
    target.Q := v;
  end SetQ;
  public static method Main(): int
    using var t1 := new Elem(3);
    using var e := new E(t1);
    var s: I<Elem> := e;
    Console.WriteLine(s.Q.V);
    using var t2 := new Elem(9);
    SetQ(s, t2);
    Console.WriteLine(s.Q.V);
    Console.WriteLine(e.Q.V);
    return 0;
  end Main;
end Program;
)BZL",
               "3\n9\n9\n");
}

// ─── T3 — primitive control: T = int, bare direct instantiation ───────────
// The erased i32 placeholder already equals the substituted type when T =
// int, so no itable specialization is ever triggered and this case is
// unaffected by both the defect and the fix — pinned so the fix's
// fallback-and-precedence ordering cannot regress the primitive case.
// Pre-fix: 9\n9\n (already correct). Post-fix: unchanged, 9\n9\n.
BEZEL_TEST(interface_generic_setter_param_type_4947, int_control_unchanged) {
    expectBoth(R"BZL(
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
    using var e := new B<int>(3);
    var s: I<int> := e;
    s.Q := 9;
    Console.WriteLine(s.Q);
    Console.WriteLine(e.Q);
    return 0;
  end Main;
end Program;
)BZL",
               "9\n9\n");
}
