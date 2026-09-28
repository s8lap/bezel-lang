// Regression tests for issue #5058 — §11.2 ("Interface declaration": a member
// matches an interface member "under the interface's substitution") / §14.1
// ("Generic declarations") / §23 ("Properties and Indexers"), v0.307.
//
// THE DEFECT (silent wrong code). A DIRECT generic instantiation
// (`new B<string>(...)` widened to `I<string>`, no wrapping non-generic
// class) gets its per-instantiation itable from `specializedVtableFor`. Its
// property / indexer accessor slot patch refused to install the specialized
// accessor whenever that accessor's LLVM function type differed from the
// erased (`T -> i32`) template's — which is precisely what a reference-typed
// `T` produces. The slot kept the erased template, so a read through
// `I<string>` returned the string handle truncated to an i32 (an empty line,
// no diagnostic) and a read through `I<Node>` then dereferenced a truncated
// pointer (SEGV at 0xc). The concrete-derived shape (`class E: B<string>`)
// was already right via patchConcreteDerivedGenericBaseItables (#4933).
//
// THE FIX. The itable accessor patch installs the specialized accessor with
// no signature-equality guard (the itab is a per-instantiation clone, and the
// interface-side dispatch types the call under the same substitution).
//
// SETTER ROWS. A plain `s.Q := v` through `I<string>` does not compile on
// this base (#4947: the call site types the value with the erased `T`, an IR
// verifier abort — fail-closed, fixed separately). The setter slot is driven
// here through a generic helper `Put<T>(x: I<T>, v: T)`: under its
// monomorphization the call site's `T` resolves to the real argument, so the
// call is correctly typed and the ONLY remaining variable is which body the
// itable SET slot holds. Pre-fix that is the erased template, which stores
// the handle as an i32.
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
// Pre-fix: "\n" (the I<string> getter slot held the erased template).
BEZEL_TEST(direct_generic_iface_accessor_slot_5058, issue_repro_getter_string) {
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
    using var e := new B<string>("e");
    var s: I<string> := e;
    Console.WriteLine(s.Q);
    return 0;
  end Main;
end Program;
)BZL",
               "e\n");
}

// ─── Getter sees a write made through the concrete receiver ───────────────
// The issue's second measurement. Pre-fix: "\n\nz\n".
BEZEL_TEST(direct_generic_iface_accessor_slot_5058, getter_string_after_concrete_write) {
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
    using var e := new B<string>("e");
    var s: I<string> := e;
    Console.WriteLine(s.Q);
    e.Q := "z";
    Console.WriteLine(s.Q);
    Console.WriteLine(e.Q);
    return 0;
  end Main;
end Program;
)BZL",
               "e\nz\nz\n");
}

// ─── Getter at a user CLASS T ─────────────────────────────────────────────
// Pre-fix: SEGV (the Node pointer truncated to i32, then `.N` at +0xc).
BEZEL_TEST(direct_generic_iface_accessor_slot_5058, getter_class_t) {
    expectBoth(R"BZL(
module M;
public interface I<T>
  property Q: T;
end I;
public class Node
  public property N: int;
  public constructor(n: int) N := n; end constructor;
end Node;
public class B<T>: I<T>
  public constructor(v: T) Q := v; end constructor;
  public property Q: T;
end B;
public class Program
  public static method Main(): int
    using var n := new Node(7);
    using var c := new B<Node>(n);
    var t: I<Node> := c;
    Console.WriteLine(t.Q.N);
    return 0;
  end Main;
end Program;
)BZL",
               "7\n");
}

// ─── Setter slot at T = string, loop-amplified ────────────────────────────
// 50 writes through the I<string> SET slot, each read back through both the
// interface and the concrete receiver. Every string crossing the slot goes
// through the specialized setter's string-ARC store, so a leak or a double
// release shows under ASan/LSan. Pre-fix the erased setter stores an i32.
BEZEL_TEST(direct_generic_iface_accessor_slot_5058, setter_string_loop50) {
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
  public static method Put<T>(x: I<T>, v: T)
    x.Q := v;
  end Put;
  public static method Main(): int
    using var e := new B<string>("e");
    var s: I<string> := e;
    var ok := 0;
    for var k in 0..<50 do
      if k mod 2 = 0 then
        Put<string>(s, "w");
      else
        Put<string>(s, "v");
      end if;
      if s.Q = e.Q then ok := ok + 1; end if;
    end for;
    Console.WriteLine(ok);
    Console.WriteLine(e.Q);
    Console.WriteLine(s.Q);
    return 0;
  end Main;
end Program;
)BZL",
               "50\nv\nv\n");
}

// ─── Setter slot at a user CLASS T ────────────────────────────────────────
// Pre-fix: the erased setter stores a truncated pointer; the concrete read
// of `.N` then faults.
BEZEL_TEST(direct_generic_iface_accessor_slot_5058, setter_class_t) {
    expectBoth(R"BZL(
module M;
public interface I<T>
  property Q: T;
end I;
public class Node
  public property N: int;
  public constructor(n: int) N := n; end constructor;
end Node;
public class B<T>: I<T>
  public constructor(v: T) Q := v; end constructor;
  public property Q: T;
end B;
public class Program
  public static method Put<T>(x: I<T>, v: T)
    x.Q := v;
  end Put;
  public static method Main(): int
    using var a := new Node(7);
    using var b := new Node(9);
    using var c := new B<Node>(a);
    var t: I<Node> := c;
    Put<Node>(t, b);
    Console.WriteLine(c.Q.N);
    Console.WriteLine(t.Q.N);
    return 0;
  end Main;
end Program;
)BZL",
               "9\n9\n");
}

// ─── Indexer getter slot through I<string> ────────────────────────────────
// Same patch, the indexer arm. Pre-fix: an empty line.
BEZEL_TEST(direct_generic_iface_accessor_slot_5058, indexer_getter_string) {
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
public class Program
  public static method Main(): int
    using var e := new B<string>("idx");
    var f: IRead<string> := e;
    Console.WriteLine(f[0]);
    return 0;
  end Main;
end Program;
)BZL",
               "idx\n");
}

// ─── Direct setter through I<string>, loop-amplified (T1) ─────────────────
// #4947's fix enables `s.Q := v` directly through the interface without a
// generic helper. The erased setter stores an i32; pre-fix: IR-verifier abort
// (call site types the value with the erased T). Post-fix: correct string-ARC
// store, verified by loop amplification + ASan.
BEZEL_TEST(direct_generic_iface_accessor_slot_5058, setter_string_direct_t1) {
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
    using var e := new B<string>("e");
    var s: I<string> := e;
    var ok := 0;
    for var k in 0..<50 do
      if k mod 2 = 0 then
        s.Q := "w";
      else
        s.Q := "v";
      end if;
      if s.Q = e.Q then ok := ok + 1; end if;
    end for;
    Console.WriteLine(ok);
    Console.WriteLine(e.Q);
    Console.WriteLine(s.Q);
    return 0;
  end Main;
end Program;
)BZL",
               "50\nv\nv\n");
}

// ─── Generic helper with differently-named type param, direct setter (T3) ─
// Proves the fix is robust to type-param-name variation: a helper `Put<U>`
// (not `Put<T>`) calling `x.Q := v` directly through `I<string>`. Pre-fix:
// IR-verifier abort (same as T1). Post-fix: correct string-ARC store.
BEZEL_TEST(direct_generic_iface_accessor_slot_5058, setter_string_direct_u_param_t3) {
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
  public static method Put<U>(x: I<U>, v: U)
    x.Q := v;
  end Put;
  public static method Main(): int
    using var e := new B<string>("e");
    var s: I<string> := e;
    Put<string>(s, "z");
    Console.WriteLine(e.Q);
    Console.WriteLine(s.Q);
    return 0;
  end Main;
end Program;
)BZL",
               "z\nz\n");
}

// ─── int control ──────────────────────────────────────────────────────────
// T = int: the erased template is signature-exact; getter and setter through
// the interface were already right and must stay so.
BEZEL_TEST(direct_generic_iface_accessor_slot_5058, int_control_getter_and_setter) {
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
  public static method Put<T>(x: I<T>, v: T)
    x.Q := v;
  end Put;
  public static method Main(): int
    using var i := new B<int>(3);
    var u: I<int> := i;
    Console.WriteLine(u.Q);
    Put<int>(u, 4);
    Console.WriteLine(i.Q);
    u.Q := 5;
    Console.WriteLine(i.Q);
    Console.WriteLine(u.Q);
    return 0;
  end Main;
end Program;
)BZL",
               "3\n4\n5\n5\n");
}
