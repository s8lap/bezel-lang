// Codegen — #8011 (§8.6 / §28.3 / §28.9, v0.353): the §8.6 argument box the
// caller mints for an `Object` parameter is released after the call when the
// callee only passes the parameter ON, as a plain call argument, to a callee
// whose own parameter does not escape.
//
// §8.6 (v0.353): "When a value type is used where `Object` (or an interface
//   implemented by the value type) is expected, the value is boxed: a
//   heap-allocated wrapper holds the value. Boxed values are runtime-managed
//   ([§28.9])."
// §28.3 (v0.353): the box minted for an argument is an unbound owned
//   temporary whose lifetime ends at the call return; a call argument is a
//   borrow for the duration of the call.
//
// THE DEFECT. #2952 releases the argument box at the post-call point only
// when ParamEscapeWalker proves the callee's parameter non-escaping. The
// walker's call arm marked EVERY parameter passed on as a call argument as
// escaping, so `Tag(5)` below leaked its 24-byte box on every call even though
// `Helper` only borrows its argument for the duration of the nested call.
//
// THE ISSUE'S OWN REPRO IS INERT. It declares `Tag<Object>(x: Object)`: since
// e4355ef9 the type parameter `Object` SHADOWS the root `Object`, so the
// parameter is a bare type parameter, no §8.6 box is minted, and nothing
// leaks (the 24 B the original reviewer measured was #8008's shadowing
// symptom). The rows below use the CORRECTED repro (a non-generic `Tag`),
// which leaks 24 B per call on the pre-fix compiler (MEASURED: LeakSanitizer,
// one `calloc(1, 24)`, zero `bz_box_release`).
//
// THE FIX defers the walker's verdict to the CALLEE: a plain positional
// argument naming the tracked parameter escapes only when the callee's
// parameter at that position escapes (`isCalleeParamNonEscaping`, recursively).
// It answers only for a callee with STATIC dispatch whose body this module
// compiles; a virtual / abstract / non-`sealed` override / interface member, an extension,
// an operator, an `async` method, an iterator, an imported callee, or a
// `ref`/`out`/`move`/named argument keeps the old escape verdict.
//
// OVER-RELEASE GUARDS. The guard rows pass the parameter to a callee that
// genuinely lets it escape (field store, return, virtual override that
// stores, recursion). They run under ASan with leak detection OFF, and READ
// the stored box after the outer call returns, so a premature release
// surfaces as a heap-use-after-free / wrong value, not as a missing leak.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// The corrected repro (the verifier's P9), looped 100x so the leak oracle is
// not one-shot: 100 x 24 B leaked before the fix.
const char* kCorrectedRepro =
    "module M8011A; "
    "public class Program "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 0..<100 do t := t + Tag(5); end for; "
    "    Console.WriteLine($\"{t}\"); "
    "    return 0; "
    "  end Main; "
    "  public static method Tag(o: Object): int "
    "    var h := Helper(o); "
    "    return h + ((o as int) ?? 0); "
    "  end Tag; "
    "  public static method Helper(o: Object): int => 7; "
    "end Program;";

// The issue-shaped body: the nested call sits inside `Console.WriteLine`.
const char* kInsideWriteLine =
    "module M8011B; "
    "public class Program "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 0..<100 do t := t + Tag(5); end for; "
    "    Console.WriteLine($\"{t}\"); "
    "    return 0; "
    "  end Main; "
    "  public static method Tag(o: Object): int "
    "    Console.WriteLine(Helper(o)); "
    "    return (o as int) ?? 0; "
    "  end Tag; "
    "  public static method Helper(o: Object): int => 7; "
    "end Program;";

// The qualified spelling `Program.Helper(o)` and a two-level chain
// Tag -> Mid -> Leaf, where Leaf reads the payload.
const char* kQualifiedChain =
    "module M8011C; "
    "public class Program "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 0..<100 do t := t + Tag(5); end for; "
    "    Console.WriteLine($\"{t}\"); "
    "    return 0; "
    "  end Main; "
    "  public static method Tag(o: Object): int => Program.Mid(o); "
    "  public static method Mid(o: Object): int "
    "    return Leaf(o) + 1; "
    "  end Mid; "
    "  public static method Leaf(o: Object): int "
    "    return (o as int) ?? 0; "
    "  end Leaf; "
    "end Program;";

// GUARD 1: the nested callee STORES its parameter into a field; the stored
// box is read after the outer call returns.
const char* kGuardFieldStore =
    "module M8011D; "
    "public class Sink "
    "  public _saved: Object; "
    "  public constructor() _saved := null; end constructor; "
    "  public method Store(v: Object): int "
    "    _saved := v; "
    "    return 1; "
    "  end Store; "
    "end Sink; "
    "public class Program "
    "  public static method Tag(s: Sink, o: Object): int "
    "    return s.Store(o); "
    "  end Tag; "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var a := Tag(s, 7); "
    "    var b := (s._saved as int) ?? 0; "
    "    var h := s._saved.ToString(); "
    "    Console.WriteLine($\"{a} {b} {h}\"); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// GUARD 2: the nested callee is VIRTUAL. The base body only reads, but the
// override that actually runs stores the parameter; the static target's body
// must not decide the release.
const char* kGuardVirtualOverride =
    "module M8011E; "
    "public class Base "
    "  public constructor() end constructor; "
    "  public virtual method Put(v: Object): int => 0; "
    "end Base; "
    "public class Keeper: Base "
    "  public _saved: Object; "
    "  public constructor() _saved := null; end constructor; "
    "  public override method Put(v: Object): int "
    "    _saved := v; "
    "    return 1; "
    "  end Put; "
    "end Keeper; "
    "public class Program "
    "  public static method Tag(b: Base, o: Object): int "
    "    return b.Put(o); "
    "  end Tag; "
    "  public static method Main(): int "
    "    using var k := new Keeper(); "
    "    var a := Tag(k, 9); "
    "    var b := (k._saved as int) ?? 0; "
    "    var h := k._saved.ToString(); "
    "    Console.WriteLine($\"{a} {b} {h}\"); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// GUARD 3: the nested callee RETURNS its parameter, and the outer callee
// stores the result in a field that outlives the call.
const char* kGuardReturnThenStore =
    "module M8011F; "
    "public class Sink "
    "  public _saved: Object; "
    "  public constructor() _saved := null; end constructor; "
    "  public method Tag(o: Object): int "
    "    _saved := Pass(o); "
    "    return 1; "
    "  end Tag; "
    "  public static method Pass(v: Object): Object => v; "
    "end Sink; "
    "public class Program "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var a := s.Tag(11); "
    "    var b := (s._saved as int) ?? 0; "
    "    var h := s._saved.ToString(); "
    "    Console.WriteLine($\"{a} {b} {h}\"); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// GUARD 4: mutual recursion between two callees that pass the parameter on;
// the analysis must terminate, and a cycle is answered conservatively.
const char* kGuardRecursion =
    "module M8011G; "
    "public class Program "
    "  public static method Even(n: int, o: Object): int "
    "    if n = 0 then return (o as int) ?? 0; end if; "
    "    return Odd(n - 1, o); "
    "  end Even; "
    "  public static method Odd(n: int, o: Object): int "
    "    if n = 0 then return 0; end if; "
    "    return Even(n - 1, o); "
    "  end Odd; "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 0..<10 do t := t + Even(4, 3); end for; "
    "    Console.WriteLine($\"{t}\"); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// GUARD 5 (the rejected review's F1, p11 looped): the nested callee captures
// its parameter in a LAMBDA stored in a field; the closure runs after the
// outer call returns. §27.5 (v0.355): "Captured variables are heap-allocated
// in a closure object", so the box outlives the call.
const char* kGuardLambdaCapture =
    "module M8011H; "
    "public class Sink "
    "  public _f: (() -> string)?; "
    "  public constructor() _f := null; end constructor; "
    "end Sink; "
    "public class Program "
    "  public static method Keep(s: Sink, o: Object): int "
    "    s._f := lambda() => o.ToString(); "
    "    return 1; "
    "  end Keep; "
    "  public static method Tag(s: Sink, o: Object): int => Keep(s, o); "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var a := Tag(s, i + 1000); "
    "      var f := s._f!; "
    "      if f() = (i + 1000).ToString() then n := n + a; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// GUARD 6 (the review's p11c): the captured parameter is read through an
// interpolation hole inside the lambda.
const char* kGuardLambdaInterp =
    "module M8011I; "
    "public class Sink "
    "  public _f: (() -> string)?; "
    "  public constructor() _f := null; end constructor; "
    "end Sink; "
    "public class Program "
    "  public static method Keep(s: Sink, o: Object): int "
    "    s._f := lambda() => $\"{o}\"; "
    "    return 1; "
    "  end Keep; "
    "  public static method Tag(s: Sink, o: Object): int => Keep(s, o); "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var a := Tag(s, i + 1000); "
    "      var f := s._f!; "
    "      if f() = (i + 1000).ToString() then n := n + a; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// GUARD 7 (the review's p11d): the captured parameter is only TYPE-TESTED
// inside the lambda. An early release is a silent wrong value here ("other"
// instead of "int"), not an ASan report, so the row pins the stdout.
const char* kGuardLambdaIsTest =
    "module M8011J; "
    "public class Sink "
    "  public _f: (() -> string)?; "
    "  public constructor() _f := null; end constructor; "
    "end Sink; "
    "public class Program "
    "  public static method Keep(s: Sink, o: Object): int "
    "    s._f := lambda() => (if o is int then \"int\" else \"other\"); "
    "    return 1; "
    "  end Keep; "
    "  public static method Tag(s: Sink, o: Object): int => Keep(s, o); "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var a := Tag(s, i + 1000); "
    "      var f := s._f!; "
    "      if f() = \"int\" then n := n + a; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// GUARD 8 (the review's p20, looped): mutual recursion whose ODD arm STORES
// the parameter. Pins the cycle's escape answer: a cycle back to a callee
// still being analysed is answered "escapes", so neither callee is cached
// non-escaping.
const char* kGuardMutualStore =
    "module M8011K; "
    "public class Sink "
    "  public _saved: Object?; "
    "  public constructor() _saved := null; end constructor; "
    "end Sink; "
    "public class Program "
    "  public static method Even(s: Sink, n: int, o: Object): int "
    "    if n = 0 then return 0; end if; "
    "    return Odd(s, n - 1, o); "
    "  end Even; "
    "  public static method Odd(s: Sink, n: int, o: Object): int "
    "    if n = 0 then s._saved := o; return 1; end if; "
    "    return Even(s, n - 1, o); "
    "  end Odd; "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var a := Even(s, 3, i + 1000); "
    "      if s._saved!.ToString() = (i + 1000).ToString() then n := n + a; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// GUARD 9: the nested callee stores its parameter through a §17.15 binding
// atom (the fix-forward of ca822b0b's bind arm, reached through a pass-on).
const char* kGuardBindStore =
    "module M8011L; "
    "public class Sink "
    "  public _saved: Object?; "
    "  public constructor() _saved := null; end constructor; "
    "end Sink; "
    "public class Program "
    "  public static method Keep(s: Sink, o: Object): int "
    "    s._saved := (var t := o; t); "
    "    return 1; "
    "  end Keep; "
    "  public static method Tag(s: Sink, o: Object): int => Keep(s, o); "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var a := Tag(s, i + 1000); "
    "      if s._saved!.ToString() = (i + 1000).ToString() then n := n + a; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// GUARD 10: the nested callee stores its parameter through a call whose
// result is DISCARDED (§17.13), reached through a pass-on.
const char* kGuardDiscardStore =
    "module M8011M; "
    "public class Sink "
    "  public _saved: Object?; "
    "  public constructor() _saved := null; end constructor; "
    "  public method Put(o: Object): int "
    "    _saved := o; "
    "    return 1; "
    "  end Put; "
    "end Sink; "
    "public class Program "
    "  public static method Keep(s: Sink, o: Object): int "
    "    _ := s.Put(o); "
    "    return 1; "
    "  end Keep; "
    "  public static method Tag(s: Sink, o: Object): int => Keep(s, o); "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      var a := Tag(s, i + 1000); "
    "      if s._saved!.ToString() = (i + 1000).ToString() then n := n + a; end if; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// RELEASE through the fix-forward's arms: the nested callee only type-tests
// its parameter inside a discard and a binding atom, so the pass-on does not
// escape and the box is released (100 x 24 B leaked before the fix).
const char* kPassOnBindDiscardRead =
    "module M8011N; "
    "public class Program "
    "  public static method Look(o: Object): int "
    "    _ := (if o is int then 1 else 0); "
    "    var n := (var t := 1; (if o is int then t else 0)); "
    "    return n; "
    "  end Look; "
    "  public static method Tag(o: Object): int => Look(o); "
    "  public static method Main(): int "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      n := n + Tag(i + 1000); "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// RELEASE through a `sealed override`: the static target's body is the one
// that runs (no further override), and it only type-tests the parameter.
const char* kPassOnSealedRead =
    "module M8011O; "
    "public class Base "
    "  public constructor() end constructor; "
    "  public virtual method Look(v: Object): int => 0; "
    "end Base; "
    "public class Leaf: Base "
    "  public constructor() end constructor; "
    "  public sealed override method Look(v: Object): int "
    "    => (if v is int then 1 else 0); "
    "end Leaf; "
    "public class Program "
    "  public static method Tag(k: Leaf, o: Object): int => k.Look(o); "
    "  public static method Main(): int "
    "    using var k := new Leaf(); "
    "    var i := 0; var n := 0; "
    "    while i < 100 do "
    "      n := n + Tag(k, i + 1000); "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program;";


// ---------------------------------------------------------------------------
// fix-forward of 6da9a1b2 (§17.4 / §26.7 / §8.6 / §28.9, v0.355): two of the
// walker's "safe read" arms answered READ for a shape that KEEPS the box, and
// the pass-on deferral then carried that verdict up through every forwarder.
//
// §17.4 (v0.355): "When the target type is a value type, `x as T` produces
//   `T?`: the boxed `Object`-or-interface input is unboxed to the value type"
//   — and otherwise "the result is additionally a *borrow* of `x` (more
//   precisely, a borrow with the same source as `x`)". So `o as IFoo` /
//   `o as Object` IS the parameter's box, and a store of it keeps the box.
// §26.7 (v0.355): "The receiver is always a borrow; extensions cannot store
//   `this`" — an extension member's `this` is the receiver reference itself.
//   fix #7968 (v0.355) closed the "the compiler does not yet reject the store
//   (#7823 / #7968)" gap: an extension body that stores its receiver is now
//   REJECTED at compile time, so the rows below that used to exercise the
//   runtime box-retention of such a store pin the rejection instead. The
//   escape verdict this walker's receiver arm carries is still load-bearing
//   for every OTHER extension-member receiver (a read-only extension member,
//   an extension property), which the walk still has to treat as an escape.
//
// The `keeps_box` rows below read the stored box AFTER the outer call
// returned, 100x, under ASan with leak detection off: a premature release is
// a heap-use-after-free / SEGV or a wrong count. MEASURED on 6da9a1b2: the
// forwarded rows printed 0 (`as IFoo`, `extension IFoo` storing `this`) or
// died in ASan (`extension Object` storing `this`; the `ToString` witness).

// B1, forwarded: `Keep` stores `o as IFoo` (a reference-target `as`).
const char* kAsIfaceStoreFwd =
    "module M8011P; "
    "public class Sink "
    "  public _saved: Object?; "
    "  public constructor() _saved := null; end constructor; "
    "end Sink; "
    "public interface IFoo "
    "  method Get(): int; "
    "end IFoo; "
    "public record R(x: int): IFoo "
    "  public method Get(): int => x; "
    "end R; "
    "public class Program "
    "  public static method Keep(s: Sink, o: Object): int "
    "    s._saved := o as IFoo; return 1; "
    "  end Keep; "
    "  public static method Tag(s: Sink, o: Object): int => Keep(s, o); "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var n := 0; "
    "    for var i in 0..<100 do "
    "      var a := Tag(s, new R(i + 1000)); "
    "      var g := s._saved as IFoo; "
    "      if g <> null and g!.Get() = i + 1000 then n := n + a; end if; "
    "    end for; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// B1, direct: the storing method is the one Main calls.
const char* kAsIfaceStoreDirect =
    "module M8011Q; "
    "public class Sink "
    "  public _saved: Object?; "
    "  public constructor() _saved := null; end constructor; "
    "end Sink; "
    "public interface IFoo "
    "  method Get(): int; "
    "end IFoo; "
    "public record R(x: int): IFoo "
    "  public method Get(): int => x; "
    "end R; "
    "public class Program "
    "  public static method Tag(s: Sink, o: Object): int "
    "    s._saved := o as IFoo; return 1; "
    "  end Tag; "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var n := 0; "
    "    for var i in 0..<100 do "
    "      var a := Tag(s, new R(i + 1000)); "
    "      var g := s._saved as IFoo; "
    "      if g <> null and g!.Get() = i + 1000 then n := n + a; end if; "
    "    end for; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// B1, forwarded, the ASan witness: the stored box is read through
// `ToString()` and through a second `as IFoo` after the outer call.
const char* kAsIfaceStoreFwdWitness =
    "module M8011R; "
    "public class Sink "
    "  public _saved: Object?; "
    "  public constructor() _saved := null; end constructor; "
    "end Sink; "
    "public interface IFoo "
    "  method Get(): int; "
    "end IFoo; "
    "public record R(x: int): IFoo "
    "  public method Get(): int => x; "
    "end R; "
    "public class Program "
    "  public static method Keep(s: Sink, o: Object): int "
    "    s._saved := o as IFoo; return 1; "
    "  end Keep; "
    "  public static method Tag(s: Sink, o: Object): int => Keep(s, o); "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var n := 0; "
    "    for var i in 0..<100 do "
    "      var a := Tag(s, new R(1005)); "
    "      var t := s._saved!.ToString(); "
    "      var f: IFoo := (s._saved as IFoo)!; "
    "      if f.Get() = 1005 and t.Length > 0 then n := n + a; end if; "
    "    end for; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// B2, forwarded: the receiver call reaches an `extension Object` member that
// stores `this`.
const char* kExtensionRecvStoreFwd =
    "module M8011S; "
    "public class Sink "
    "  public _saved: Object?; "
    "  public constructor() _saved := null; end constructor; "
    "end Sink; "
    "public extension Object "
    "  public method KeepIn(s: Sink): int "
    "    s._saved := this; return 1; "
    "  end KeepIn; "
    "end extension; "
    "public class Program "
    "  public static method Inner(s: Sink, o: Object): int => o.KeepIn(s); "
    "  public static method Tag(s: Sink, o: Object): int => Inner(s, o); "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var n := 0; "
    "    for var i in 0..<100 do "
    "      var a := Tag(s, i + 1000); "
    "      if s._saved!.ToString() = (i + 1000).ToString() then "
    "        n := n + a; "
    "      end if; "
    "    end for; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// B2, direct.
const char* kExtensionRecvStoreDirect =
    "module M8011T; "
    "public class Sink "
    "  public _saved: Object?; "
    "  public constructor() _saved := null; end constructor; "
    "end Sink; "
    "public extension Object "
    "  public method KeepIn(s: Sink): int "
    "    s._saved := this; return 1; "
    "  end KeepIn; "
    "end extension; "
    "public class Program "
    "  public static method Tag(s: Sink, o: Object): int => o.KeepIn(s); "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var n := 0; "
    "    for var i in 0..<100 do "
    "      var a := Tag(s, i + 1000); "
    "      if s._saved!.ToString() = (i + 1000).ToString() then "
    "        n := n + a; "
    "      end if; "
    "    end for; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// B2, forwarded, interface-typed parameter: `extension IFoo` stores `this`.
const char* kIfaceExtensionStoreThisFwd =
    "module M8011U; "
    "public class Sink "
    "  public _saved: Object?; "
    "  public constructor() _saved := null; end constructor; "
    "end Sink; "
    "public interface IFoo "
    "  method Get(): int; "
    "end IFoo; "
    "public record R(x: int): IFoo "
    "  public method Get(): int => x; "
    "end R; "
    "public extension IFoo "
    "  public method KeepIn(s: Sink): int "
    "    s._saved := this; return 1; "
    "  end KeepIn; "
    "end extension; "
    "public class Program "
    "  public static method Inner(s: Sink, f: IFoo): int => f.KeepIn(s); "
    "  public static method Tag(s: Sink, f: IFoo): int => Inner(s, f); "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var n := 0; "
    "    for var i in 0..<100 do "
    "      var a := Tag(s, new R(i + 1000)); "
    "      var g := s._saved as IFoo; "
    "      if g <> null and g!.Get() = i + 1000 then n := n + a; end if; "
    "    end for; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// Admitting twin: a reference-target `as` that is only compared against
// `null` keeps nothing, so the pass-on still releases the box.
const char* kAsIfaceNullTestFwd =
    "module M8011V; "
    "public interface IFoo "
    "  method Get(): int; "
    "end IFoo; "
    "public record R(x: int): IFoo "
    "  public method Get(): int => x; "
    "end R; "
    "public class Program "
    "  public static method Peek(o: Object): int "
    "    if (o as IFoo) <> null then return 1; end if; "
    "    return 0; "
    "  end Peek; "
    "  public static method Tag(o: Object): int => Peek(o); "
    "  public static method Main(): int "
    "    var n := 0; "
    "    for var i in 0..<100 do n := n + Tag(new R(i + 1000)); end for; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// Admitting twin: an `extension Object` member that only unboxes `this`
// is reached through the receiver; the walker does not follow `this` into
// the extension body, so the pass-on keeps the box (the leak direction —
// the lenient leg only pins that nothing is freed early).
const char* kExtensionRecvReadFwd =
    "module M8011W; "
    "public extension Object "
    "  public method Peek(): int => (this as int) ?? 0; "
    "end extension; "
    "public class Program "
    "  public static method Inner(o: Object): int => o.Peek(); "
    "  public static method Tag(o: Object): int => Inner(o); "
    "  public static method Main(): int "
    "    var n := 0; "
    "    for var i in 0..<100 do n := n + Tag(i + 1); end for; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program;";


// B2 via an extension PROPERTY (§26.3, expression-bodied): `o.Self` IS the
// box, stored by the forwarded callee.
const char* kExtensionPropertyThisStoreFwd =
    "module M8011X; "
    "public class Sink "
    "  public _saved: Object?; "
    "  public constructor() _saved := null; end constructor; "
    "end Sink; "
    "public extension Object "
    "  public property Self: Object => this; "
    "end extension; "
    "public class Program "
    "  public static method Keep(s: Sink, o: Object): int "
    "    s._saved := o.Self; return 1; "
    "  end Keep; "
    "  public static method Tag(s: Sink, o: Object): int => Keep(s, o); "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var n := 0; "
    "    for var i in 0..<100 do "
    "      var a := Tag(s, i + 1000); "
    "      if s._saved!.ToString() = (i + 1000).ToString() then "
    "        n := n + a; "
    "      end if; "
    "    end for; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// B1 + B2: the receiver is `(o as IFoo)!` and the member reached is an
// `extension IFoo` member storing `this`.
const char* kAsIfaceBangExtensionRecvStoreFwd =
    "module M8011Y; "
    "public class Sink "
    "  public _saved: Object?; "
    "  public constructor() _saved := null; end constructor; "
    "end Sink; "
    "public interface IFoo "
    "  method Get(): int; "
    "end IFoo; "
    "public record R(x: int): IFoo "
    "  public method Get(): int => x; "
    "end R; "
    "public extension IFoo "
    "  public method KeepIn(s: Sink): int "
    "    s._saved := this; return 1; "
    "  end KeepIn; "
    "end extension; "
    "public class Program "
    "  public static method Keep(s: Sink, o: Object): int => "
    "    (o as IFoo)!.KeepIn(s); "
    "  public static method Tag(s: Sink, o: Object): int => Keep(s, o); "
    "  public static method Main(): int "
    "    using var s := new Sink(); "
    "    var n := 0; "
    "    for var i in 0..<100 do "
    "      var a := Tag(s, new R(i + 1000)); "
    "      var g := s._saved as IFoo; "
    "      if g <> null and g!.Get() = i + 1000 then n := n + a; end if; "
    "    end for; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// Admitting twin: the receiver `(o as IFoo)!` reaches a DECLARED interface
// member, which is the receiver read `o.Get()` is: released.
const char* kAsIfaceBangRecvReadFwd =
    "module M8011Z; "
    "public interface IFoo "
    "  method Get(): int; "
    "end IFoo; "
    "public record R(x: int): IFoo "
    "  public method Get(): int => x; "
    "end R; "
    "public class Program "
    "  public static method Peek(o: Object): int => (o as IFoo)!.Get(); "
    "  public static method Tag(o: Object): int => Peek(o); "
    "  public static method Main(): int "
    "    var n := 0; "
    "    for var i in 0..<100 do n := n + Tag(new R(i + 1)) - i; end for; "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end Program;";

}  // namespace

// fix #7968 (§26.7, v0.355): the extension-receiver store check rejects an
// extension body that stores its receiver, so the rows below that used to
// exercise the RUNTIME box-retention of such a store now pin the
// REJECTION instead. Asserted on the diagnostic TEXT: `runAot`'s
// WIFSIGNALED path synthesizes an Error diagnostic for a runtime crash too,
// so `!r.ran` alone cannot distinguish a genuine rejection from a segfault
// (tests/README.md).
namespace {

bool rejectedByExtensionReceiverRule(const char* program) {
    JitResult r = runAotChecked(program);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("§26.7") != std::string::npos &&
            d.message.find("must not store its receiver") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// per §8.6 / §28.3, v0.353 — the corrected repro: the argument box of
// `Tag(5)` is released after each call (100 x 24 B leaked before the fix).
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           corrected_repro_pass_on_releases_box_spec_8_6) {
    JitResult plain = runAot(kCorrectedRepro);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    BEZEL_EXPECT_EQ(plain.stdoutText, std::string("1200\n"));
    JitResult r = runAotChecked(kCorrectedRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1200\n"));
}

// per §8.6 / §28.3, v0.353 — the issue's body shape: the nested call is an
// argument of `Console.WriteLine`.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           pass_on_inside_writeline_releases_box_spec_8_6) {
    std::string expect;
    for (int i = 0; i < 100; ++i) expect += "7\n";
    expect += "500\n";
    JitResult plain = runAot(kInsideWriteLine);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.stdoutText, expect);
    JitResult r = runAotChecked(kInsideWriteLine);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expect);
}

// per §8.6 / §28.3, v0.353 — a qualified call and a two-level chain: the
// non-escape verdict is transitive.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           qualified_two_level_chain_releases_box_spec_8_6) {
    JitResult plain = runAot(kQualifiedChain);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.stdoutText, std::string("600\n"));
    JitResult r = runAotChecked(kQualifiedChain);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("600\n"));
}

// per §28.3, v0.353 — GUARD: the nested callee stores the parameter; the box
// is kept alive (no use-after-free; leak detection off).
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           guard_nested_field_store_keeps_box_spec_28_3) {
    JitResult r = runAotCheckedLenient(kGuardFieldStore);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1 7 7\n"));
}

// per §28.3 / §12, v0.353 — GUARD: a virtual nested callee whose override
// stores the parameter; the base body does not decide the release.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           guard_virtual_override_store_keeps_box_spec_28_3) {
    JitResult r = runAotCheckedLenient(kGuardVirtualOverride);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1 9 9\n"));
}

// per §28.3, v0.353 — GUARD: the nested callee returns the parameter and the
// result is stored in a field; the box is kept alive.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           guard_nested_return_then_store_keeps_box_spec_28_3) {
    JitResult r = runAotCheckedLenient(kGuardReturnThenStore);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1 11 11\n"));
}

// per §28.3, v0.353 — GUARD: mutual recursion terminates and computes the
// right value.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           guard_mutual_recursion_terminates_spec_28_3) {
    JitResult r = runAotCheckedLenient(kGuardRecursion);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("30\n"));
}

// Rows below added with the rebase onto the fix-forward of ca822b0b
// (§8.6 / §27.5 / §12.1, v0.355). Every guard loops 100x inside the program
// and pins the exact stdout; under the ASan leg an early release is a
// heap-use-after-free (or, for the type-test row, a wrong count).

// per §27.5 / §8.6, v0.355 — GUARD (review F1): a lambda in the nested callee
// captures the parameter; the closure runs after the outer call returns.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           guard_nested_lambda_capture_keeps_box_spec_27_5) {
    JitResult r = runAotCheckedLenient(kGuardLambdaCapture);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// per §27.5 / §8.6, v0.355 — GUARD: the captured parameter is read through an
// interpolation hole inside the lambda.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           guard_nested_lambda_interpolation_keeps_box_spec_27_5) {
    JitResult r = runAotCheckedLenient(kGuardLambdaInterp);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// per §27.5 / §8.6, v0.355 — GUARD: the captured parameter is only type-tested
// inside the lambda; an early release reads a freed box's type tag.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           guard_nested_lambda_type_test_keeps_box_spec_27_5) {
    JitResult r = runAotCheckedLenient(kGuardLambdaIsTest);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// per §8.6, v0.355 — GUARD: mutual recursion whose arm STORES the parameter;
// the cycle is answered "escapes".
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           guard_mutual_recursion_store_keeps_box_spec_8_6) {
    JitResult r = runAotCheckedLenient(kGuardMutualStore);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// per §17.15 / §8.6, v0.355 — GUARD: the nested callee stores the parameter
// through a binding atom.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           guard_nested_bind_atom_store_keeps_box_spec_17_15) {
    JitResult r = runAotCheckedLenient(kGuardBindStore);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// per §17.13 / §8.6, v0.355 — GUARD: the nested callee stores the parameter
// through a call whose result is discarded.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           guard_nested_discard_store_keeps_box_spec_17_13) {
    JitResult r = runAotCheckedLenient(kGuardDiscardStore);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// per §8.6, v0.355 — the pass-on reaches a callee that only type-tests the
// parameter inside a discard and a binding atom: the box is released.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           pass_on_bind_discard_read_releases_box_spec_8_6) {
    JitResult plain = runAot(kPassOnBindDiscardRead);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.stdoutText, std::string("100\n"));
    JitResult r = runAotChecked(kPassOnBindDiscardRead);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// per §12.1 / §8.6, v0.355 — the pass-on reaches a `sealed override`, whose
// body is the one that runs and only type-tests the parameter: released.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           pass_on_sealed_override_read_releases_box_spec_12_1) {
    JitResult plain = runAot(kPassOnSealedRead);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.stdoutText, std::string("100\n"));
    JitResult r = runAotChecked(kPassOnSealedRead);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// per §17.4 / §8.6, v0.355 — the forwarded callee stores `o as IFoo`: the
// result is a borrow of the same box, so the box is kept (no use-after-free;
// every read-back correct; leak detection off).
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           as_iface_store_fwd_keeps_box_spec_17_4) {
    JitResult r = runAotCheckedLenient(kAsIfaceStoreFwd);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// per §17.4 / §8.6, v0.355 — the same store in the directly called method.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           as_iface_store_direct_keeps_box_spec_17_4) {
    JitResult r = runAotCheckedLenient(kAsIfaceStoreDirect);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// per §17.4 / §28.9, v0.355 — the ASan witness: the stored box survives a
// `ToString()` and a second `as` after the outer call.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           as_iface_store_fwd_tostring_witness_keeps_box_spec_17_4) {
    JitResult r = runAotCheckedLenient(kAsIfaceStoreFwdWitness);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// per §26.7 / §8.6, v0.355 — fix #7968: the forwarded receiver call reaches
// an `extension Object` member whose `this` (the box) is stored, and §26.7
// forbids exactly that store ("The receiver is always a borrow; extensions
// cannot store `this`, return it as `owned`, or dispose it."). The row now
// pins the REJECTION; before it, the box was kept at run time and the store
// left a dangling borrow behind (§28.3's residual trust).
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           extension_recv_store_fwd_rejected_spec_26_7) {
    BEZEL_EXPECT_TRUE(rejectedByExtensionReceiverRule(kExtensionRecvStoreFwd));
}

// per §26.7 / §8.6, v0.355 — fix #7968: the same extension call in the
// directly called method.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           extension_recv_store_direct_rejected_spec_26_7) {
    BEZEL_EXPECT_TRUE(
        rejectedByExtensionReceiverRule(kExtensionRecvStoreDirect));
}

// per §26.7 / §8.6, v0.355 — fix #7968: an interface-typed parameter whose
// `extension IFoo` member stores `this`, through a forwarder.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           iface_extension_store_this_fwd_rejected_spec_26_7) {
    BEZEL_EXPECT_TRUE(
        rejectedByExtensionReceiverRule(kIfaceExtensionStoreThisFwd));
}

// per §17.4 / §8.6, v0.355 — admitting twin: `(o as IFoo) <> null` in the
// forwarded callee keeps nothing, so the box is still released (no leak).
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           as_iface_null_test_fwd_releases_box_spec_17_4) {
    JitResult plain = runAot(kAsIfaceNullTestFwd);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.stdoutText, std::string("100\n"));
    JitResult r = runAotChecked(kAsIfaceNullTestFwd);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// per §26.7 / §8.6, v0.355 — admitting twin: a read-only extension member
// reached through the receiver computes the right value and frees nothing
// early (the box is kept, the conservative direction; leak detection off).
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           extension_recv_read_fwd_no_early_release_spec_26_7) {
    JitResult r = runAotCheckedLenient(kExtensionRecvReadFwd);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5050\n"));
}

// per §26.3 / §26.7 / §8.6, v0.355 — an extension PROPERTY whose body is
// `this`, read on the forwarded parameter and stored: the box is kept.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           extension_property_this_store_fwd_keeps_box_spec_26_7) {
    JitResult r = runAotCheckedLenient(kExtensionPropertyThisStoreFwd);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// per §17.4 / §26.7 / §8.6, v0.355 — fix #7968: the receiver `(o as IFoo)!`
// reaches an `extension IFoo` member storing `this`, and §26.7 forbids that
// store.
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           as_iface_bang_extension_recv_store_fwd_rejected_spec_26_7) {
    BEZEL_EXPECT_TRUE(
        rejectedByExtensionReceiverRule(kAsIfaceBangExtensionRecvStoreFwd));
}

// per §17.4 / §17.8 / §8.6, v0.355 — admitting twin: `(o as IFoo)!.Get()`
// in the forwarded callee is a receiver read of a declared member, so the
// box is still released (no leak).
BEZEL_TEST(codegen_param_escape_arg_box_8011,
           as_iface_bang_recv_read_fwd_releases_box_spec_17_4) {
    JitResult plain = runAot(kAsIfaceBangRecvReadFwd);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.stdoutText, std::string("100\n"));
    JitResult r = runAotChecked(kAsIfaceBangRecvReadFwd);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}
