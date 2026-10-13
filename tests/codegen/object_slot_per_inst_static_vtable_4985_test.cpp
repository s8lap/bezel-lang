// Regression tests for issue #4985 — §8.5 ("ToString") / §8.3 ("`Equals` and
// `GetHashCode`") / §14.1 ("Generic declarations") / §2.8.3 rule 4, v0.307.
//
// THE DEFECT (silent wrong code, measured). A virtual method override on a
// generic class `B<T>` that touches a per-instantiation static field —
// including the §8.5/§8.3 ToString/Equals/GetHashCode overrides that occupy
// the FIXED Object slots 3/4/5 — still dispatched to the ERASED `B<T>`
// template body through the vtable. `specializedVtableFor`'s condition (e)
// — Object slots 3/4/5 — gated re-lowering on
// `instanceLayoutDiffersFromErasedTemplate(user->parent)`, the same predicate
// #4966 removed from condition (c) (virtual METHOD slots) and #4984
// removed from condition (d) (virtual PROPERTY/INDEXER accessor slots).
// The predicate has the same blind spot on (e) as on (c)/(d): B<T> has no
// instance fields, every LLVM struct layout agrees, the predicate is
// therefore false, but the thunk still calls `methodFns_[user]` — the
// ERASED `T -> i32` template body — non-virtually. The body reads `B<T>._c`
// (a STATIC field whose global name is `bz$M.B$_c`, the placeholder) and
// re-emitted under the active substitution at `bz$M.B$T$_c`. The erased
// body GEPs the per-instantiation cell for `T -> i32` rather than the cell
// for the receiver's actual T. `class D<T>: B<T>` overriding `ToString():
// string` to `$"<D{B<T>._c}>"` at T = int: the vtable's erased thunk
// forwarded to the template body that GEPs `bz$M.B$_c`, so every print
// showed `<D0>` even after `B<int>._c := 10`. The defect is not in the
// LAYOUT — it is in the BODY. Layout-equal does not mean body-equal.
//
// THE FIX. `Emitter::specializedVtableFor` (LlvmBackend_Setup.cpp) now re-
// builds every FIXED Object slot 3/4/5 thunk of the GENERIC class that
// still holds the ERASED template thunk, under the active substitution,
// with no `instanceLayoutDiffersFromErasedTemplate` gate. The signature-
// ABI guard (`concFnTy != mit->second->getFunctionType()`) stays — a
// specialization whose signature MOVES under the substitution cannot be
// forwarded to from the thunk's fixed-argument call site. A class with
// NO user override never reaches here at all (its thunk is the
// §8.5/§8.3 compiler-generated default, which reads no field and is
// therefore never wrong). The ToString branch is the (e) arm; the Equals
// and GetHashCode arms are exercised by the same fix and pin the same
// path. This is the FIXED-Object-slot twin of #4966 (cb405b15) for
// condition (c) and #4984 for condition (d); all three were named as
// follow-ups in #4966's commit_msg.
//
// Every row asserts exact stdout; each runs on the plain AOT path and again
// under generated-code ASan (runAotChecked).

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

// ─── ToString override: B<T> : I<T> / Object dispatch ──────────────────────
// Pre-fix: "<D0>\n<D0>\n<D0>\n" — the vtable's erased thunk forwarded to
// the erased template body that read `bz$M.B$_c`, so every call saw 0.
// Post-fix: "<D10>\n<D20>\n<D20>\n" — the thunk now points at a per-T
// specialized body that reads `bz$M.B$int$_c`, so the per-inst value is
// observed through both the B<int>-typed receiver AND the Object-typed
// receiver (the second §8.5 path of every string-interpolation hole that
// dispatches virtually).
BEZEL_TEST(object_slot_per_inst_static_vtable_4985,
           tostring_via_base_then_object) {
    expectBoth(R"BZL(
module M;
public class B<T>
  public static _c: int := 0;
  public override method ToString(): string => $"<{B<T>._c}>";
end B;
public class D<T>: B<T>
  public override method ToString(): string => $"<D{B<T>._c}>";
end D;
public class Program
  public static method Main(): int
    var d := new D<int>();
    var b: B<int> := d;
    B<int>._c := 10;
    Console.WriteLine(b.ToString());
    B<int>._c := 20;
    Console.WriteLine(b.ToString());
    var o: Object := b;
    Console.WriteLine(o.ToString());
    return 0;
  end Main;
end Program;
)BZL",
               "<D10>\n<D20>\n<D20>\n");
}

// ─── Two instantiations: B<int> and B<string> each observe their own cell
// Pre-fix: "<D0>\n<D0>\n0\n0\n" — both ToString calls read the same erased
// template cell. Post-fix: "<D11>\n<D22>\n11\n22\n" — each instantiation's
// ToString reads its own per-inst cell, and the B<int>._c / B<string>._c
// reads confirm the specialization writes to the per-inst globals.
BEZEL_TEST(object_slot_per_inst_static_vtable_4985,
           tostring_two_instantiations_distinct_cells) {
    expectBoth(R"BZL(
module M;
public class B<T>
  public static _c: int := 0;
  public override method ToString(): string => $"<D{B<T>._c}>";
end B;
public class Program
  public static method Main(): int
    var bi := new B<int>();
    var bs := new B<string>();
    B<int>._c := 11;
    B<string>._c := 22;
    Console.WriteLine(bi.ToString());
    Console.WriteLine(bs.ToString());
    Console.WriteLine(B<int>._c);
    Console.WriteLine(B<string>._c);
    return 0;
  end Main;
end Program;
)BZL",
               "<D11>\n<D22>\n11\n22\n");
}

// ─── Equals override (slot 4) ─────────────────────────────────────────────
// The same condition (e) predicate covers Equals. Pre-fix: every Equals
// dispatch through a virtual receiver reached the erased B<T>.Equals body
// that GEPs the placeholder cell; even though Equals here is structurally
// stateless (the body returns true iff `other = self`), the rewritten
// `methodFns_[user]` reference itself was wrong — the SAME per-inst static
// question applies. The expected values pin that the override is reached
// AND that the type-test decision is per-instantiation (§12.7's
// per-instantiation rule for a `T`-named target).
//
// Per §8.3 ("`Equals` and `GetHashCode`"): "A type that overrides `Equals`
// must also override `GetHashCode`. The compiler enforces this." Both B<T>
// and D<T> override Equals, so each must also override GetHashCode — added
// here as `=> 0` because this row pins the Equals dispatch path, not the
// hash semantics.
//
// Per §12.7 (type tests): `is T` is decided per instantiation when `T` is a
// type parameter (§3.6 rule 2 applied here) — `is D<T>` at D<int>'s slot
// becomes `is D<int>`, and both `d1` and `d2` are D<int> instances, so the
// body returns true for both calls (the bodies test TYPE-equality, not
// identity, so two distinct D<int> instances compare equal). Per stdlib
// §2.1 the `bool` Console.WriteLine overload renders the canonical
// "True"/"False" spelling (not "true"/"false"); the original row carried
// the wrong-shape expected — both lines are "True".
BEZEL_TEST(object_slot_per_inst_static_vtable_4985,
           equals_override_reaches_per_inst_cell) {
    expectBoth(R"BZL(
module M;
public class B<T>
  public static _c: int := 0;
  public override method Equals(other: Object?): bool
    if (other is null) then return false; end if;
    if (other is B<T>) then return true; end if;
    return false;
  end Equals;
  public override method GetHashCode(): int => 0;
end B;
public class D<T>: B<T>
  public override method Equals(other: Object?): bool
    if (other is null) then return false; end if;
    if (other is D<T>) then return true; end if;
    return false;
  end Equals;
  public override method GetHashCode(): int => 0;
end D;
public class Program
  public static method Main(): int
    var d1 := new D<int>();
    var d2 := new D<int>();
    var o1: Object := d1;
    var o2: Object := d2;
    Console.WriteLine(o1.Equals(o1));
    Console.WriteLine(o1.Equals(o2));
    return 0;
  end Main;
end Program;
)BZL",
               // Per §12.7 (per-instantiation type test on `T`): both d1
               // and d2 are D<int>, so `is D<T>` at T=int is true for
               // both — the bodies test type equality, not identity.
               // Stdlib §2.1 `bool` WriteLine overload renders "True"/
               // "False" with canonical case.
               "True\nTrue\n");
}

// ─── GetHashCode override (slot 5) ────────────────────────────────────────
// Same condition (e) predicate covers GetHashCode. Pre-fix the erased body
// GEPs the placeholder static cell; post-fix the per-inst cell is read.
// Two distinct B<int> instances must hash identically because the body's
// per-inst static IS the source of the hash — pinning that the override is
// reached, not the default.
//
// The receivers are typed `Object`, NOT `D<int>`. That is load-bearing, and it
// is what makes this row a FAIL-WITHOUT pin rather than a vacuous pass. With
// a statically-typed `var d1 := new D<int>()` receiver the call site knows the
// exact override and devirtualizes to a direct call, so it never reads the
// vtable slot and passes on pre-fix code too — MEASURED on the R26 lane
// 4985-cherry re-derivation: the `D<int>`-typed and `B<int>`-typed variants
// both printed "101\n101\n" with the (e) gate re-inserted, i.e. they passed
// against the UNFIXED compiler. Widening to `Object` puts the call back
// through the vtable's FIXED Object slot 5, and the same program then prints
// "1\n1\n" pre-fix (the erased template body GEP'd the untouched placeholder
// cell `bz$M.B$_c`, so `B<T>._c + 1` read 0 + 1) against "101\n101\n"
// post-fix (the per-inst thunk reads `bz$M.B$int$_c`).
BEZEL_TEST(object_slot_per_inst_static_vtable_4985,
           gethashcode_override_reaches_per_inst_cell) {
    expectBoth(R"BZL(
module M;
public class B<T>
  public static _c: int := 0;
  public override method GetHashCode(): int => B<T>._c;
end B;
public class D<T>: B<T>
  public override method GetHashCode(): int => B<T>._c + 1;
end D;
public class Program
  public static method Main(): int
    var d1 := new D<int>();
    var d2 := new D<int>();
    B<int>._c := 100;
    var o1: Object := d1;
    var o2: Object := d2;
    Console.WriteLine(o1.GetHashCode());
    Console.WriteLine(o2.GetHashCode());
    return 0;
  end Main;
end Program;
)BZL",
               "101\n101\n");
}

// ─── Control: a class with NO override — the §8.5 compiler-generated
// default always returns the type's qualified name (§8.5 "For classes: the
// type's qualified name"), independent of any per-inst state. The pre-fix
// predicate (`instanceLayoutDiffersFromErasedTemplate`) was false for every
// generic class without instance fields, and the no-override case was
// already correct — the `classSlotToStringImpl` etc. helpers return
// nullptr when there is no user override, so the loop above never sees
// these classes. This row pins that the no-override path is unchanged.
//
// §8.5 v0.313, "ToString", classes bullet, exact wording: "For classes:
// the type's **qualified name** — the module-path-qualified rendering stdlib
// [§2.13](Bezel_Standard_Library.md#sec-the-type-class) defines for
// `Type.FullName` (`"BankingApp.BankAccount"`, and `"System.Object"` for
// `new Object()`, [§8.1](#sec-definition); a nested class carries its
// enclosing-type path — `"BankingApp.Ledger.Entry"` for `Entry` declared
// inside `Ledger`, [§11.6](#sec-nested-types); a generic instantiation
// renders per stdlib §2.13 rule 3, type arguments recursing)." For
// `class B<T>` declared in `module M`, the qualified name is `M.B` — the
// module-path-qualified rendering of the declaring type. Stdlib §2.13 rule
// 1 ("a named non-generic type renders its declared identifier (Name) / its
// module-qualified path (FullName)") is the surface the §8.5 default
// consumes; rule 3 covers constructions nested as type arguments, not the
// top-level receiver of ToString.
BEZEL_TEST(object_slot_per_inst_static_vtable_4985,
           control_no_override_compiler_default) {
    expectBoth(R"BZL(
module M;
public class B<T>
  public static _c: int := 0;
end B;
public class Program
  public static method Main(): int
    var b := new B<int>();
    var o: Object := b;
    Console.WriteLine(o.ToString());
    return 0;
  end Main;
end Program;
)BZL",
               // Per §8.5 v0.313 qualified-name rule, the default ToString
               // for `class B<T>` declared in `module M` is `M.B`.
               "M.B\n");
}