// Regression tests for issue #5884 — §22.12 rule 1 ("a type's static
// initializer runs exactly once process-wide") / §22.13 / §3.6 rule 2
// (per-instantiation evaluation for generics), v0.334.
//
// THE DEFECT (silent wrong runtime answer, measured). #1513 gave every
// generic instantiation its OWN LinkOnceODR `ensureinit` + `sfinit` pair, so
// that instantiation's static cell is initialized exactly once, at the cell's
// first touch, under its own once-guard. The #709 loop in
// `emitStaticConstructorBody` — the per-TYPE sfinit's "also store into every
// per-instantiation global" twin store — kept running, so from #1513 onward
// TWO INDEPENDENT once-guards wrote the SAME per-instantiation cell:
//
//   * `bz$M.B$string$sfinit` under `bz$M.B$string$initstate` (§22.12 rule 1),
//     and
//   * `bz$B$sfinit` under the per-TYPE `bz$M.B$initstate`, whose #709 loop
//     re-stores the `:= 0` initializer into `bz$M.B$string$_c`.
//
// Whichever guard runs second re-initializes a cell the first already
// initialized. Measured on `class B<T> { static _c: int := 0; virtual
// VBump() ... }` with `class D<T>: B<T>`: the per-instantiation sfinit ran at
// the first `VBump` (state 0 -> 2), and the per-TYPE sfinit ran afterwards at
// the `new`, re-storing 0 over a cell that had reached 20 — so a later
// `k.VBump()` returned 10 instead of 30 and the following `B<string>._c`
// read agreed with the clobbered cell. No diagnostic at any severity; exit
// code 0 throughout.
//
// THE SHAPE THAT MATTERS — and it is NOT the upcast. The issue as filed
// blamed the `D<string>` -> `B<string>` upcast, and the upcast is NOT the
// trigger: deleting the upcast line leaves the failure exactly intact. The
// trigger is a direct `new B<string>()` AFTER the cell has been touched, which
// is the site that reaches the per-TYPE guard. The first row below is the
// no-upcast form, and it is the row that pins the real discriminator — a fix
// made at the upcast or at a call site would leave it red.
//
// THE FIX. `emitStaticConstructorBody`'s #709 loop stands down for a field the
// instantiation's own once-guarded sfinit already stored; the shapes that
// sfinit declines (collection-literal, inline-array initializers) keep the #709
// loop as their only writer.
//
// The rows assert exact stdout; each runs on the plain AOT path and again
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

// ─── The real discriminator: NO upcast anywhere ─────────────────────────
// `D<string>`'s vtable dispatches through B<string>'s OWN cell twice, a direct
// `B<string>._c` read agrees (20), then `new B<string>()` reaches the per-TYPE
// guard and re-stores the `:= 0` initializer over the live cell. Pre-fix:
// "10\n20\n20\n10\n10\n". Post-fix: "10\n20\n20\n30\n30\n".
//
// This is the same program as #4966's `probe_q5_d_generic_derived` with the
// upcast line deleted, and it is a REGRESSION row in its own right: the
// upcast-free form is what shows the defect is not in the vtable or the
// upcast path at all.
BEZEL_TEST(generic_static_init_runs_once_per_process_5884,
           no_upcast_direct_base_construction_after_derived) {
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
    Console.WriteLine(d.VBump());
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

// ─── Two instantiations, EACH reconstructed after being touched ─────────
// A CONTROL, and it PASSES on the pre-fix and post-fix compilers alike —
// measured: 1 passed / 2 failed without the fix, and this is the one that
// passed. It is here to close a specific trap, not to pin the defect: the
// per-TYPE sfinit re-stores at the FIRST `new`, which in this program happens
// BEFORE any `VBump`, so nothing is clobbered and the row is already
// correct. What it does pin is cell INDEPENDENCE — `B<int>` and `B<string>`
// are two distinct cells (§22.12: "There is one cell per declaring
// instantiation") — so a "fix" that made every static access read the most
// recently initialized cell would pass both failing rows above and fail
// here, where `B<int>` and `B<string>` must each reach 20 independently.
BEZEL_TEST(generic_static_init_runs_once_per_process_5884,
           two_instantiations_each_reconstructed_keep_distinct_cells) {
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
public class Program
  public static method Main(): int
    using var di := new B<int>();
    Console.WriteLine(di.VBump());
    using var ds := new B<string>();
    Console.WriteLine(ds.VBump());
    Console.WriteLine(B<int>._c);
    Console.WriteLine(B<string>._c);
    using var di2 := new B<int>();
    using var ds2 := new B<string>();
    Console.WriteLine(di2.VBump());
    Console.WriteLine(ds2.VBump());
    Console.WriteLine(B<int>._c);
    Console.WriteLine(B<string>._c);
    return 0;
  end Main;
end Program;
)BZL",
               "10\n10\n10\n10\n20\n20\n20\n20\n");
}

// ─── A non-virtual static read survives the direct construction ─────────
// CONTROL for the non-virtual path: `B<int>.s := 200` then `new B<int>()`.
// Pre-fix the per-TYPE sfinit re-stored `:= 0` over the written cell at the
// `new`, so `bi.GetNoVirt()` read 0 instead of 200. Post-fix it reads the
// value the program wrote. This is the same shape as #4966's
// `control_nonvirtual_dispatch`, restated here so the #5884 file carries its
// own control.
BEZEL_TEST(generic_static_init_runs_once_per_process_5884,
           control_nonvirtual_read_after_construction) {
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
    var bi: B<int> := new B<int>();
    Console.WriteLine(bi.GetNoVirt());
    Console.WriteLine(B<int>.s);
    return 0;
  end Main;
end Program;
)BZL",
               "200\n200\n");
}