// Regression tests for issue #4984 — §14.1 ("Generic declarations") / §23
// ("Properties and Indexers") / §2.8.3 rule 4, v0.307.
//
// THE DEFECT (silent wrong code, measured). A virtual PROPERTY accessor on a
// generic class `B<T>` whose body touches a per-instantiation static field
// still dispatched to the ERASED `B<T>` template accessor through the
// vtable. `specializedVtableFor`'s condition (d) — virtual PROPERTY/INDEXER
// accessor slots — gated re-lowering on
// `instanceLayoutDiffersFromErasedTemplate(impl->parent)`, the same predicate
// #4966 removed from condition (c) (virtual METHOD slots). The predicate
// has the same blind spot on (d) as it had on (c): B<T> has no instance
// fields, every LLVM struct layout agrees, the predicate is therefore false,
// but the accessor's body still reads `B<T>._c` — a STATIC field whose
// global name is `bz$M.B$_c` (placeholder) and is re-emitted under the
// active substitution at `bz$M.B$T$_c` — and the ERASED body GEPs the
// per-instantiation cell for `T -> i32` rather than the cell for the
// receiver's actual T. `class D<T>: B<T>` overriding `virtual property
// Val: int` whose setter writes `B<T>._c := value + 1`, at T = int:
// the vtable's erased accessor wrote `bz$M.B$_c` (the placeholder cell
// that belongs to no instantiation), and the per-inst `B<int>._c` reads
// printed 0 instead of the expected 21. The defect is not in the LAYOUT
// — it is in the BODY. Layout-equal does not mean body-equal.
//
// THE FIX. `Emitter::specializedVtableFor` (LlvmBackend_Setup.cpp) now re-
// lowers EVERY inherited virtual-property / indexer accessor slot of the
// GENERIC class that still holds the ERASED template accessor, under the
// active substitution, with no `instanceLayoutDiffersFromErasedTemplate`
// gate. The signature-ABI guard
// (`specFn->getFunctionType() != canon->getFunctionType()`) stays — a
// specialization whose signature MOVES under the substitution (`virtual
// property Item: T`) cannot go in the fixed-ABI slot, and the indirect
// call at the receiver's static property type would mistype it. This is
// the property/indexer twin of #4966 (cb405b15) for condition (c) and the
// same #4966 commit_msg named condition (d) as out-of-grant. Mirror of
// #4985 for the fixed Object slots 3/4/5. The specialization is shared
// with the call-site one through `buildSpecializedAccessorFn`'s cache.
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

// ─── Issue's repro: D<T>: B<T> at T = int with virtual PROPERTY ─────────
// Pre-fix: "11\n21\n21\n0\n" — the third number is `B<int>._c` reading the
// UNTOUCHED per-instantiation cell, because the vtable's erased accessor
// wrote to the placeholder `bz$M.B$_c`. Post-fix: "11\n21\n21\n" — the
// per-inst cell observes the writes because the specialized accessor writes
// to `bz$M.B$int$_c`. (Three WriteLines, three expected lines — the row's
// source issues exactly three: `b.Val`, `d.Val`, `B<int>._c`.)
BEZEL_TEST(virtual_property_accessor_per_inst_static_vtable_4984,
           property_set_dispatch_writes_per_inst_cell) {
    expectBoth(R"BZL(
module M;
public class B<T>
  public static _c: int := 0;
  public virtual property Val: int
    get => _c;
    set => do _c := value; end;
  end Val;
end B;
public class D<T>: B<T>
  public override property Val: int
    get => B<T>._c;
    set => do B<T>._c := value + 1; end;
  end Val;
end D;
public class Program
  public static method Main(): int
    var d := new D<int>();
    var b: B<int> := d;
    b.Val := 10;
    Console.WriteLine(b.Val);
    d.Val := 20;
    Console.WriteLine(d.Val);
    Console.WriteLine(B<int>._c);
    return 0;
  end Main;
end Program;
)BZL",
               // 3 WriteLines × 3 expected lines. Row 1 originally carried a
               // stray 4th value; the source issues exactly three (b.Val,
               // d.Val, B<int>._c).
               "11\n21\n21\n");
}

// ─── B<int>.Val.get through B<int>-typed receiver ───────────────────────
// The getter reads B<T>._c through the vtable. Pre-fix the vtable held the
// erased accessor that read the placeholder cell, so a write through the
// direct path then a virtual read returned the untouched per-inst value.
// Post-fix the virtual getter reads the per-inst cell the direct setter
// wrote.
BEZEL_TEST(virtual_property_accessor_per_inst_static_vtable_4984,
           property_get_after_direct_set_reads_per_inst_cell) {
    expectBoth(R"BZL(
module M;
public class B<T>
  public static s: int := 0;
  public virtual property Val: int
    get => s;
    set => do s := value; end;
  end Val;
end B;
public class Program
  public static method Main(): int
    var b := new B<int>();
    var v: B<int> := b;
    B<int>.s := 100;
    Console.WriteLine(v.Val);
    v.Val := 200;
    Console.WriteLine(v.Val);
    Console.WriteLine(B<int>.s);
    return 0;
  end Main;
end Program;
)BZL",
               "100\n200\n200\n");
}

// ─── Two instantiations: B<int> and B<string> ───────────────────────────
// A virtual setter / getter on each instantiation must observe ITS OWN
// per-instantiation static, not the erased placeholder cell. Pre-fix both
// dispatch paths landed on `bz$M.B$_c`, so writes to B<int> and B<string>
// both modified the same cell. Post-fix each instantiation has its own
// per-inst cell.
BEZEL_TEST(virtual_property_accessor_per_inst_static_vtable_4984,
           two_instantiations_distinct_accessor_cells) {
    expectBoth(R"BZL(
module M;
public class B<T>
  public static _c: int := 0;
  public virtual property Val: int
    get => _c;
    set => do _c := value; end;
  end Val;
end B;
public class Program
  public static method Main(): int
    var bi := new B<int>();
    var bs := new B<string>();
    bi.Val := 11;
    bs.Val := 22;
    Console.WriteLine(bi.Val);
    Console.WriteLine(bs.Val);
    Console.WriteLine(B<int>._c);
    Console.WriteLine(B<string>._c);
    return 0;
  end Main;
end Program;
)BZL",
               "11\n22\n11\n22\n");
}

// ─── Generic derived D<T>: B<T> at two different T ─────────────────────
// D<int> and D<string> each have their own specialized vtable (separate
// per-(class, T) `vt$D$specN`). The overridden setter on each must write
// ITS OWN B<T>._c cell. Pre-fix both dispatch paths landed on the
// template cell; post-fix each instantiation observes its own cell.
//
// Per §12.1 / §23.3, an override replaces the base member: D<T>'s setter
// writes `B<T>._c := value + 100` and B<T>'s `set => _c := value + 1` is
// NOT invoked — there is no implicit base call. So `di.Val := 10` lands
// 110 (not 111) and `ds.Val := 20` lands 120 (not 121).
BEZEL_TEST(virtual_property_accessor_per_inst_static_vtable_4984,
           generic_derived_dispatches_via_accessor) {
    expectBoth(R"BZL(
module M;
public class B<T>
  public static _c: int := 0;
  public virtual property Val: int
    get => _c;
    set => do _c := value + 1; end;
  end Val;
end B;
public class D<T>: B<T>
  public override property Val: int
    get => B<T>._c;
    set => do B<T>._c := value + 100; end;
  end Val;
end D;
public class Program
  public static method Main(): int
    var di := new D<int>();
    var ds := new D<string>();
    di.Val := 10;
    ds.Val := 20;
    Console.WriteLine(di.Val);
    Console.WriteLine(ds.Val);
    Console.WriteLine(B<int>._c);
    Console.WriteLine(B<string>._c);
    return 0;
  end Main;
end Program;
)BZL",
               // Per §12.1 (override replaces base) and §23.3 (setter
               // body executes verbatim): D's +100 write replaces B's +1,
               // so each value reads back as value + 100 = 110 / 120.
               "110\n120\n110\n120\n");
}

// ─── Control: a non-virtual property on B<T> already worked correctly ───
// Direct dispatch through `b.ValNV` reads / writes the per-inst cell. This
// row is a CONTROL — it passes on the pre-fix and post-fix compilers
// alike, and pins that the re-lower machinery did not break the direct
// path.
BEZEL_TEST(virtual_property_accessor_per_inst_static_vtable_4984,
           control_nonvirtual_accessor) {
    expectBoth(R"BZL(
module M;
public class B<T>
  public static s: int := 0;
  public property ValNV: int
    get => s;
    set => do s := value; end;
  end ValNV;
end B;
public class Program
  public static method Main(): int
    var bi := new B<int>();
    var bs := new B<string>();
    B<int>.s := 200;
    B<string>.s := 300;
    Console.WriteLine(bi.ValNV);
    Console.WriteLine(bs.ValNV);
    return 0;
  end Main;
end Program;
)BZL",
               "200\n300\n");
}