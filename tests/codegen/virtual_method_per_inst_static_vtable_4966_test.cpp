// Regression tests for issue #4966 — §22.12 ("Each distinct instantiation has
// its own static state") / §14.1 ("Generic declarations": a base clause is an
// instantiation site) / §2.8.3 rule 4, v0.307.
//
// THE DEFECT (silent wrong code, measured). A VIRTUAL method on a generic
// class `B<T>` that touches a per-instantiation static field still dispatched
// to the ERASED `B<T>` template body through the vtable. #2906's
// `specializedVtableFor` re-lowered such a slot only when `T` moved the
// INSTANCE LAYOUT, on the premise that the erased body is otherwise exact. It
// is not: `T` can reach the body without touching the layout, and the one
// measured case is a §22.12 per-instantiation static. `B<T>.VBump()` doing
// `B<T>._c := B<T>._c + 10` compiles, erased, against the placeholder-level
// cell `bz$M.B$_c`, which belongs to no instantiation. The probe q5 program
// (`class D<T>: B<T>` at T = string) printed `10 20 0 30 0` for `10 20 20 30
// 30` — every dispatch through `B<T>`'s vtable wrote the WRONG cell, and the
// `B<string>._c` reads saw the untouched per-inst cell. `instanceLayoutDiffers`
// can't see the static difference: every LLVM instance-layout type agrees
// (B<T> has no instance fields, only a static).
//
// THE FIX. `Emitter::specializedVtableFor` (LlvmBackend_Setup.cpp) now re-
// lowers EVERY inherited virtual-method slot of the GENERIC class that still
// holds the erased template, under the active substitution, with no
// `instanceLayoutDiffersFromErasedTemplate` gate. The same argument #4950 made
// for the itable path, with the itable's `patchConcreteDerivedGenericBaseItables`
// replaced by vtable's `specializedVtableFor` (which serves the GENERIC class's
// OWN specialized vtable). The specialization is shared with the call-site
// one through buildSpecializedMethodFn's cache.
//
// SCOPE OF THIS FIX. This PR closes the GENERIC class's OWN specialized
// vtable case. The CONCRETE-DERIVED (non-generic `class E: B<string>`) vtable
// case is the #4950 itable fix's direct vtable analogue and is left for a
// follow-up — it requires the same member-function declaration + call-site
// addition to LlvmBackend_impl.hpp that #4950 made for the itable, which the
// lane grant forbids. The four rows below pin the GENERIC specialization path
// (B<T>'s own specialized vtable, including D<T>: B<T> at T = string) so the
// probe q5 repro stays green; the literal `class E: B<string>` case is not
// pinned here.
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

// ─── Probe q5 repro: D<T>: B<T> at T = string ────────────────────────────
// Pre-fix: "10\n20\n0\n30\n0\n" (every dispatch through B<T>'s vtable wrote
// the erased placeholder cell bz$M.B$_c, so the B<string>._c reads at lines 3
// and 5 returned 0). Post-fix: every VBump observes B<string>'s own cell.
BEZEL_TEST(virtual_method_per_inst_static_vtable_4966, probe_q5_d_generic_derived) {
    expectBoth(R"BZL(
module M;
public class B<T>
  public static _c: int := 0;
  public constructor() end constructor;
  public virtual method VBump(): int
    B<T>._c := B<T>._c + 10;
    return B<T>._c;
  end VBump;
end B;
public class D<T>: B<T>
  public constructor() base(); end constructor;
end D;
public class Program
  public static method Main(): int
    using var d := new D<string>();
    var b: B<string> := d;
    Console.WriteLine(b.VBump());
    Console.WriteLine(d.VBump());
    Console.WriteLine(B<string>._c);
    using var k := new B<string>();
    Console.WriteLine(k.VBump());
    Console.WriteLine(B<string>._c);
    return 0;
  end Main;
end Program;
)BZL",
               "10\n20\n20\n30\n30\n");
}

// ─── B<string>.s := 100 then b.Get() returns 100 ────────────────────────
// A single instantiation; the virtual Get() reads B<T>.s. Pre-fix the vtable
// held the erased body that read bz$M.B$_c, so b.Get() returned 0 (the
// untouched template cell). Post-fix the vtable slot is re-lowered under
// {T -> string} and reads bz$M.B$string$_c, returning 100.
BEZEL_TEST(virtual_method_per_inst_static_vtable_4966, single_instantiation_direct_write_then_dispatch) {
    expectBoth(R"BZL(
module M;
public class B<T>
  public static s: int := 0;
  public constructor() end constructor;
  public virtual method Get(): int => B<T>.s;
end B;
public class Program
  public static method Main(): int
    using var b := new B<string>();
    B<string>.s := 100;
    Console.WriteLine(b.Get());
    return 0;
  end Main;
end Program;
)BZL",
               "100\n");
}

// ─── Two instantiations: B<int>.s and B<string>.s are distinct cells ────
// A virtual Get() on each instantiation must read ITS OWN cell, not the
// erased template cell. Pre-fix both calls returned 0 (template cell, never
// written). Post-fix one writes to bz$M.B$int$_c, the other to
// bz$M.B$string$_c, each Get() reads back its own value.
BEZEL_TEST(virtual_method_per_inst_static_vtable_4966, two_instantiations_distinct_cells) {
    expectBoth(R"BZL(
module M;
public class B<T>
  public static s: int := 0;
  public constructor() end constructor;
  public virtual method Get(): int => B<T>.s;
end B;
public class Program
  public static method Main(): int
    using var bi := new B<int>();
    using var bs := new B<string>();
    B<int>.s := 11;
    B<string>.s := 22;
    Console.WriteLine(bi.Get());
    Console.WriteLine(bs.Get());
    B<int>.s := 111;
    B<string>.s := 222;
    Console.WriteLine(bi.Get());
    Console.WriteLine(bs.Get());
    return 0;
  end Main;
end Program;
)BZL",
               "11\n22\n111\n222\n");
}

// ─── Generic derived D<T>: B<T> at two different T ──────────────────────
// D<int> and D<string> each have their own specialized vtable (separate
// per-(class, T) `vt$D$specN`). Each instantiation's VBump must advance ITS
// OWN B<T>._c cell. Pre-fix both dispatch paths landed on bz$M.B$_c, so
// `B<int>._c` and `B<string>._c` BOTH showed the cumulative count.
// Post-fix each instantiation observes its own cell.
BEZEL_TEST(virtual_method_per_inst_static_vtable_4966, generic_derived_dispatches_with_through) {
    expectBoth(R"BZL(
module M;
public class B<T>
  public static _c: int := 0;
  public constructor() end constructor;
  public virtual method VBump(): int
    B<T>._c := B<T>._c + 10;
    return B<T>._c;
  end VBump;
end B;
public class D<T>: B<T>
  public constructor() base(); end constructor;
end D;
public class Program
  public static method Main(): int
    using var di := new D<int>();
    using var ds := new D<string>();
    Console.WriteLine(di.VBump());
    Console.WriteLine(di.VBump());
    Console.WriteLine(ds.VBump());
    Console.WriteLine(B<int>._c);
    Console.WriteLine(B<string>._c);
    return 0;
  end Main;
end Program;
)BZL",
               "10\n20\n10\n20\n10\n");
}

// ─── Control: a non-virtual method on B<T> already worked correctly ──────
// Direct dispatch through B<int>.GetNoVirt() reads the per-inst cell. This
// row is a CONTROL — it passes on the pre-fix and post-fix compilers alike,
// and pins that the re-lower machinery did not break the direct path.
BEZEL_TEST(virtual_method_per_inst_static_vtable_4966, control_nonvirtual_dispatch) {
    expectBoth(R"BZL(
module M;
public class B<T>
  public static s: int := 0;
  public constructor() end constructor;
  public method GetNoVirt(): int => B<T>.s;
end B;
public class Program
  public static method Main(): int
    B<int>.s := 200;
    B<string>.s := 300;
    var bi: B<int> := new B<int>();
    var bs: B<string> := new B<string>();
    Console.WriteLine(bi.GetNoVirt());
    Console.WriteLine(bs.GetNoVirt());
    return 0;
  end Main;
end Program;
)BZL",
               "200\n300\n");
}