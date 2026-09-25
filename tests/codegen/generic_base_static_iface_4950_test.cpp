// Regression tests for issue #4950 — §22.12 ("Each distinct instantiation has
// its own static state") / §14.1 ("Generic declarations": a base clause is an
// instantiation site) / §11.2 (a member implements an interface member "under
// the interface's substitution") / §2.8.3 rule 4, v0.307.
//
// THE DEFECT (silent wrong code). A NON-GENERIC `class E: B<string>`, with
// `B<T>: I<T>` and `B<T>.Bump()` incrementing the per-instantiation static
// `B<T>._c`, published an itable for `I` whose `Bump` slot still held the
// ERASED `B<T>` template body. #4933 re-lowers such a slot only when `T`
// moves the slot's signature or the instance layout, and `Bump(): int` on a
// field-less class moves neither — yet the erased body reads and writes the
// placeholder-level cell `bz$M.B$_c`, which belongs to no instantiation, and
// its ensureinit runs the template static initializer, which re-zeroes the
// instantiation cells. The issue's program printed `1, 1, 0, 0` for
// `1, 2, 2, 0`. The `int` twin (`F: B<int>`) shared the placeholder cell the
// same way even though every LLVM type agrees.
//
// THE FIX. `Emitter::patchConcreteDerivedGenericBaseItables`
// (LlvmBackend_Layout.cpp) now re-lowers EVERY itable method / accessor slot
// of a non-generic class that still holds a generic ancestor's erased
// template, under that ancestor's substitution, with no signature/layout
// prefilter.
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

// ─── The issue's own repro, verbatim ──────────────────────────────────────
// Pre-fix: "1\n1\n0\n0\n".
BEZEL_TEST(generic_base_static_iface_4950, issue_repro_direct_then_iface) {
    expectBoth(R"BZL(
module M;
public interface I<T>
  method Bump(): int;
end I;
public class B<T>: I<T>
  public static _c: int := 0;
  public constructor() end constructor;
  public method Bump(): int
    B<T>._c := B<T>._c + 1;
    return B<T>._c;
  end Bump;
end B;
public class E: B<string>
  public constructor() base(); end constructor;
end E;
public class Program
  public static method Main(): int
    using var e := new E();
    Console.WriteLine(e.Bump());
    var s: I<string> := e;
    Console.WriteLine(s.Bump());
    Console.WriteLine(B<string>._c);
    Console.WriteLine(B<int>._c);
    return 0;
  end Main;
end Program;
)BZL",
               "1\n2\n2\n0\n");
}

// ─── Direct, via B<string>, via I<string>: one cell ───────────────────────
// All three faces of the same object must advance B<string>'s one cell.
// Pre-fix the I<string> call bumped the placeholder cell and re-zeroed
// B<string>._c.
BEZEL_TEST(generic_base_static_iface_4950, direct_base_and_iface_share_one_cell) {
    expectBoth(R"BZL(
module M;
public interface I<T>
  method Bump(): int;
end I;
public class B<T>: I<T>
  public static _c: int := 0;
  public constructor() end constructor;
  public method Bump(): int
    B<T>._c := B<T>._c + 1;
    return B<T>._c;
  end Bump;
end B;
public class E: B<string>
  public constructor() base(); end constructor;
end E;
public class Program
  public static method Main(): int
    using var e := new E();
    Console.WriteLine(e.Bump());
    var b: B<string> := e;
    Console.WriteLine(b.Bump());
    var s: I<string> := e;
    Console.WriteLine(s.Bump());
    Console.WriteLine(e.Bump());
    Console.WriteLine(B<string>._c);
    Console.WriteLine(B<long>._c);
    return 0;
  end Main;
end Program;
)BZL",
               "1\n2\n3\n4\n4\n0\n");
}

// ─── A generic D<T>: B<T> at string shares the cell with E: B<string> ─────
// `D<string>` goes through specializedVtableFor (already correct); `E`
// goes through the non-generic patch. §22.12 / §2.8.3 rule 4: both are
// B<string>, so both must reach the SAME cell.
BEZEL_TEST(generic_base_static_iface_4950, generic_derived_and_nongeneric_derived_share_cell) {
    expectBoth(R"BZL(
module M;
public interface I<T>
  method Bump(): int;
end I;
public class B<T>: I<T>
  public static _c: int := 0;
  public constructor() end constructor;
  public method Bump(): int
    B<T>._c := B<T>._c + 1;
    return B<T>._c;
  end Bump;
end B;
public class D<T>: B<T>
  public constructor() base(); end constructor;
end D;
public class E: B<string>
  public constructor() base(); end constructor;
end E;
public class Program
  public static method Main(): int
    using var d := new D<string>();
    var t: I<string> := d;
    Console.WriteLine(t.Bump());
    Console.WriteLine(d.Bump());
    using var e := new E();
    var s: I<string> := e;
    Console.WriteLine(s.Bump());
    Console.WriteLine(t.Bump());
    Console.WriteLine(B<string>._c);
    return 0;
  end Main;
end Program;
)BZL",
               "1\n2\n3\n4\n4\n");
}

// ─── Two instantiations stay separate (string and the int twin) ───────────
// Pre-fix both itables held the erased body, so E's and F's interface calls
// shared ONE placeholder cell (and the int twin is wrong too: the placeholder
// cell `bz$M.B$_c` is not `bz$M.B$int$_c` even though every type agrees).
BEZEL_TEST(generic_base_static_iface_4950, two_instantiations_kept_separate) {
    expectBoth(R"BZL(
module M;
public interface I<T>
  method Bump(): int;
end I;
public class B<T>: I<T>
  public static _c: int := 0;
  public constructor() end constructor;
  public method Bump(): int
    B<T>._c := B<T>._c + 1;
    return B<T>._c;
  end Bump;
end B;
public class E: B<string>
  public constructor() base(); end constructor;
end E;
public class F: B<int>
  public constructor() base(); end constructor;
end F;
public class Program
  public static method Main(): int
    using var e := new E();
    using var f := new F();
    var s: I<string> := e;
    var t: I<int> := f;
    Console.WriteLine(s.Bump());
    Console.WriteLine(t.Bump());
    Console.WriteLine(s.Bump());
    Console.WriteLine(f.Bump());
    Console.WriteLine(t.Bump());
    Console.WriteLine(B<string>._c);
    Console.WriteLine(B<int>._c);
    Console.WriteLine(B<bool>._c);
    return 0;
  end Main;
end Program;
)BZL",
               "1\n1\n2\n2\n3\n2\n3\n0\n");
}

// ─── Interface PROPERTY slot reading the static ───────────────────────────
// The accessor slot had the same prefilter (`T` erases to a non-i32 type or
// the layout moves); `Count: int` on a field-less class tripped neither, so
// `s.Count` read the placeholder cell.
BEZEL_TEST(generic_base_static_iface_4950, iface_property_reads_instantiation_static) {
    expectBoth(R"BZL(
module M;
public interface I<T>
  method Bump(): int;
  readonly property Count: int;
end I;
public class B<T>: I<T>
  public static _c: int := 0;
  public constructor() end constructor;
  public method Bump(): int
    B<T>._c := B<T>._c + 1;
    return B<T>._c;
  end Bump;
  public property Count: int => B<T>._c;
end B;
public class E: B<string>
  public constructor() base(); end constructor;
end E;
public class F: B<int>
  public constructor() base(); end constructor;
end F;
public class Program
  public static method Main(): int
    using var e := new E();
    using var f := new F();
    e.Bump();
    e.Bump();
    f.Bump();
    var s: I<string> := e;
    var t: I<int> := f;
    Console.WriteLine(s.Count);
    Console.WriteLine(t.Count);
    Console.WriteLine(e.Count);
    return 0;
  end Main;
end Program;
)BZL",
               "2\n1\n2\n");
}

// ─── Control: an INSTANCE counter, no static state ────────────────────────
// Correct before and after the fix — pins that re-lowering every slot does
// not disturb ordinary instance-field access through the interface.
BEZEL_TEST(generic_base_static_iface_4950, control_instance_field_counter) {
    expectBoth(R"BZL(
module M;
public interface I<T>
  method Bump(): int;
end I;
public class B<T>: I<T>
  private _n: int := 0;
  public constructor() end constructor;
  public method Bump(): int
    _n := _n + 1;
    return _n;
  end Bump;
end B;
public class E: B<string>
  public constructor() base(); end constructor;
end E;
public class Program
  public static method Main(): int
    using var e := new E();
    using var e2 := new E();
    Console.WriteLine(e.Bump());
    var s: I<string> := e;
    Console.WriteLine(s.Bump());
    var s2: I<string> := e2;
    Console.WriteLine(s2.Bump());
    Console.WriteLine(e.Bump());
    return 0;
  end Main;
end Program;
)BZL",
               "1\n2\n1\n3\n");
}
