// Regression tests for issue #4933 — §11.2 ("Interface declaration": a member
// matches an interface member "under the interface's substitution") / §14.1
// ("Generic declarations": a base clause is an instantiation site) / §12.1,
// v0.307.
//
// THE DEFECT (silent wrong code). A NON-GENERIC class that derives from a
// generic base at a reference-type argument (`class E: B<string>`, with
// `B<T>: I<T>`) published an itable for `I` whose slot pointed at the ERASED
// `B<T>` template body (`T` lowered as the i32 placeholder). A call through
// the `I<string>` receiver therefore read the 8-byte string handle back as an
// i32 and printed an empty line, with no diagnostic. The direct `e.Get()` and
// the `B<string>`-typed call were already correct (they specialize through
// computeAncestorGenericSubst at the call site), and so was the `int` twin
// (the erased body is exact at T = int). `specializedVtableFor` — the repair
// for a GENERIC class's per-instantiation itable — never runs for `E`,
// because no substitution is active at `new E()`.
//
// THE FIX. `Emitter::patchConcreteDerivedGenericBaseItables`
// (LlvmBackend_Layout.cpp), run once after the method bodies are emitted,
// re-lowers each itable method/property/indexer slot of a non-generic class
// whose implementation is declared on a generic ancestor, under the
// ancestor substitution the base-clause chain supplies.
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
// Pre-fix: "e\n\ne\n" (the I<string> line is empty).
BEZEL_TEST(generic_base_iface_dispatch_string_4933, issue_repro_method_through_iface) {
    expectBoth(R"BZL(
module M;
public interface I<T>
  method Get(): T;
end I;
public class B<T>: I<T>
  private _v: T;
  public constructor(v: T) _v := v; end constructor;
  public method Get(): T => _v;
end B;
public class E: B<string>
  public constructor() base("e"); end constructor;
end E;
public class Program
  public static method Main(): int
    using var e := new E();
    Console.WriteLine(e.Get());
    var s: I<string> := e;
    Console.WriteLine(s.Get());
    var b: B<string> := e;
    Console.WriteLine(b.Get());
    return 0;
  end Main;
end Program;
)BZL",
               "e\ne\ne\n");
}

// ─── Method vs property; block-bodied method ──────────────────────────────
// The interface PROPERTY slot had the same erased-template fill as the
// method slot. Pre-fix: the two I<string> lines are empty.
BEZEL_TEST(generic_base_iface_dispatch_string_4933, method_and_property_through_iface) {
    expectBoth(R"BZL(
module M;
public interface I<T>
  method Get(): T;
  readonly property P: T;
end I;
public class B<T>: I<T>
  private _v: T;
  public constructor(v: T) _v := v; end constructor;
  public method Get(): T
    var t: T := _v;
    return t;
  end Get;
  public property P: T => _v;
end B;
public class E: B<string>
  public constructor() base("e"); end constructor;
end E;
public class Program
  public static method Main(): int
    using var e := new E();
    var s: I<string> := e;
    Console.WriteLine(s.Get());
    Console.WriteLine(s.P);
    var b: B<string> := e;
    Console.WriteLine(b.Get());
    Console.WriteLine(b.P);
    return 0;
  end Main;
end Program;
)BZL",
               "e\ne\ne\ne\n");
}

// ─── Two levels: F: E: B<string> ──────────────────────────────────────────
// The non-generic grandchild publishes its own itable; its slots must be
// specialized through the full base chain. Pre-fix: both lines empty.
BEZEL_TEST(generic_base_iface_dispatch_string_4933, two_level_nongeneric_chain) {
    expectBoth(R"BZL(
module M;
public interface I<T>
  method Get(): T;
  readonly property P: T;
end I;
public class B<T>: I<T>
  private _v: T;
  public constructor(v: T) _v := v; end constructor;
  public method Get(): T => _v;
  public property P: T => _v;
end B;
public class E: B<string>
  public constructor() base("e"); end constructor;
end E;
public class F: E
  public constructor() base(); end constructor;
end F;
public class Program
  public static method Main(): int
    using var f := new F();
    var s: I<string> := f;
    Console.WriteLine(s.Get());
    Console.WriteLine(s.P);
    return 0;
  end Main;
end Program;
)BZL",
               "e\ne\n");
}

// ─── B<string> implements I<T> via ITS OWN base, and via a generic middle ──
// `A<T>: I<T>` declares the implementation; `B<T>: A<T>` inherits it; `E`
// derives from `B<string>`, and `G` from `Mid<string>: B<U>` — the
// substitution must compose through every generic link. Pre-fix: every
// I<string> line is empty.
BEZEL_TEST(generic_base_iface_dispatch_string_4933, impl_on_grandbase_and_generic_middle) {
    expectBoth(R"BZL(
module M;
public interface I<T>
  method Get(): T;
  readonly property P: T;
end I;
public class A<T>: I<T>
  private _v: T;
  public constructor(v: T) _v := v; end constructor;
  public method Get(): T => _v;
  public property P: T => _v;
end A;
public class B<T>: A<T>
  public constructor(v: T) base(v); end constructor;
end B;
public class Mid<U>: B<U>
  public constructor(v: U) base(v); end constructor;
end Mid;
public class E: B<string>
  public constructor() base("e"); end constructor;
end E;
public class G: Mid<string>
  public constructor() base("g"); end constructor;
end G;
public class Program
  public static method Show(s: I<string>)
    Console.WriteLine(s.Get());
    Console.WriteLine(s.P);
  end Show;
  public static method Main(): int
    using var e := new E();
    Show(e);
    using var g := new G();
    Show(g);
    return 0;
  end Main;
end Program;
)BZL",
               "e\ne\ng\ng\n");
}

// ─── Indexer slot through an I<string> receiver ───────────────────────────
// The interface INDEXER getter slot carried the same erased-template fill.
// Pre-fix: the IRead<string> line is empty.
BEZEL_TEST(generic_base_iface_dispatch_string_4933, indexer_through_iface) {
    expectBoth(R"BZL(
module M;
public interface IRead<T>
  readonly property [i: int]: T;
end IRead;
public class B<T>: IRead<T>
  private _v: T;
  public constructor(v: T) _v := v; end constructor;
  public property [i: int]: T => _v;
end B;
public class E: B<string>
  public constructor() base("idx"); end constructor;
end E;
public class Program
  public static method Main(): int
    using var e := new E();
    var f: IRead<string> := e;
    Console.WriteLine(f[0]);
    return 0;
  end Main;
end Program;
)BZL",
               "idx\n");
}

// ─── String parameter AND return through the interface, 50 iterations ─────
// `Put(v: T): T` stores a string and returns the old one; every string that
// crosses the slot goes through the specialized body's string-ARC store and
// return. Loop-amplified (50 round trips) so a leak or a double release in
// that path shows under ASan/LSan. Pre-fix the slot receives the string
// handle as an i32 and returns an i32, so the stored field is truncated and
// the output is wrong. (The arguments are literals, not interpolated
// temporaries: storing a fresh temporary through a `T`-typed field is a
// separate, pre-existing defect that reproduces with no interface and no
// derived class at all — filed on its own, see the #4933 handback.)
BEZEL_TEST(generic_base_iface_dispatch_string_4933, string_param_and_return_loop50) {
    expectBoth(R"BZL(
module M;
public interface I<T>
  method Get(): T;
  method Put(v: T): T;
end I;
public class B<T>: I<T>
  private _v: T;
  public constructor(v: T) _v := v; end constructor;
  public method Get(): T => _v;
  public method Put(v: T): T
    var old: T := _v;
    _v := v;
    return old;
  end Put;
end B;
public class E: B<string>
  public constructor() base("start"); end constructor;
end E;
public class Program
  public static method Main(): int
    using var e := new E();
    var s: I<string> := e;
    var n := 0;
    for var i in 0..<50 do
      if i mod 2 = 0 then
        n := n + s.Put("ab").Length;
      else
        n := n + s.Put("xyz").Length;
      end;
    end;
    Console.WriteLine(s.Get());
    Console.WriteLine(n);
    return 0;
  end Main;
end Program;
)BZL",
               // old values: "start" (5), then 25 x "ab" (2) and 24 x "xyz" (3)
               "xyz\n127\n");
}

// ─── int control: the erased body is exact at T = int ─────────────────────
// Passes on both sides of the fix; pins that the int twin is unchanged.
BEZEL_TEST(generic_base_iface_dispatch_string_4933, int_control_unchanged) {
    expectBoth(R"BZL(
module M;
public interface I<T>
  method Get(): T;
  method Put(v: T): T;
  readonly property P: T;
end I;
public class B<T>: I<T>
  private _v: T;
  public constructor(v: T) _v := v; end constructor;
  public method Get(): T => _v;
  public method Put(v: T): T
    var old: T := _v;
    _v := v;
    return old;
  end Put;
  public property P: T => _v;
end B;
public class N: B<int>
  public constructor() base(42); end constructor;
end N;
public class Program
  public static method Main(): int
    using var n := new N();
    var s: I<int> := n;
    Console.WriteLine(s.Get());
    Console.WriteLine(s.P);
    Console.WriteLine(s.Put(7));
    Console.WriteLine(s.Get());
    return 0;
  end Main;
end Program;
)BZL",
               "42\n42\n42\n7\n");
}
