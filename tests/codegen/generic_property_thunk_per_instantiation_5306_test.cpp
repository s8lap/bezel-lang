// fix #5306 (§14.1 "Generic declarations" / §27.6 "Method references" /
// §27.7.1 "Equality of function values", v0.313): a `T`-typed property (or
// any property) of a generic class, referenced in function-value position
// (§27.6: "A property reference produces the getter"), at TWO OR MORE
// instantiations in one program.
//
// THE DEFECT. `makePropertyThunk` (src/codegen/LlvmBackend_Layout.cpp)
// memoized its `pthunk$N` per `sema::PropertySymbol*` (`propertyThunks_`),
// but the thunk is per-INSTANTIATION code: its signature (`i32 (ptr)` at
// `Holder<int>`, `ptr (ptr)` at `Holder<string>`) and its body (every field
// GEP is typed against that instantiation's `class.Holder$specN`) are both
// lowered under the receiver's §14.1 substitution. The first instantiation
// to form a reference froze its thunk for every later one. Before #5283's
// hoist (ef32951c) the signature was the erased `i32` placeholder, so most
// shapes failed IR verification; after it, the first emission is correct
// for ITS instantiation and the reuse is a SILENT wrong value (issue #5306,
// found reviewing ef32951c).
//
// THE FIX keys the memo by (symbol, receiver LLVM type, substitution), the
// substitution in the canonical form `accessorSpecTagFor` (#2928) and
// buildSpecializedMethodFn (#1947) use: name-sorted (LLVM type,
// `typeRefIdentityKey`) pairs. Two references at the SAME instantiation
// still share one thunk (§27.7.1, #1544).
//
// EVERY multi-instantiation row below asserts values a shared thunk cannot
// produce; the per-row ef32951c output is recorded at each row (MEASURED,
// `BEZEL_DUMP_IR=1` + run, with the thunk count). Every row except the
// control also fails on r23-c28 (pre-#5283: IR verification failure or the
// same shared-thunk wrong value).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// a_int_str.bzl
constexpr const char* k_a_int_str =
    "module M;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public property w: int;\n"
    "  public constructor(x: T, y: int)\n"
    "    v := x;\n"
    "    w := y;\n"
    "  end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hi := new Holder<int>(3, 11);\n"
    "    var gi: () -> int := hi.v;\n"
    "    var a := gi();\n"
    "    var hs := new Holder<string>(\"z\", 22);\n"
    "    var gs: () -> string := hs.v;\n"
    "    var b := gs();\n"
    "    Console.WriteLine(a);\n"
    "    Console.WriteLine(b);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// b_str_int.bzl
constexpr const char* k_b_str_int =
    "module M;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public property w: int;\n"
    "  public constructor(x: T, y: int)\n"
    "    v := x;\n"
    "    w := y;\n"
    "  end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hs := new Holder<string>(\"z\", 22);\n"
    "    var gs: () -> string := hs.v;\n"
    "    var b := gs();\n"
    "    var hi := new Holder<int>(3, 11);\n"
    "    var gi: () -> int := hi.v;\n"
    "    var a := gi();\n"
    "    Console.WriteLine(b);\n"
    "    Console.WriteLine(a);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// c_sibling.bzl
constexpr const char* k_c_sibling =
    "module M;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public property w: int;\n"
    "  public constructor(x: T, y: int)\n"
    "    v := x;\n"
    "    w := y;\n"
    "  end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hi := new Holder<int>(3, 11);\n"
    "    var gi: () -> int := hi.w;\n"
    "    var a := gi();\n"
    "    var hs := new Holder<string>(\"z\", 22);\n"
    "    var gs: () -> int := hs.w;\n"
    "    var b := gs();\n"
    "    Console.WriteLine(a);\n"
    "    Console.WriteLine(b);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// d_rec_cls.bzl
constexpr const char* k_d_rec_cls =
    "module M;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public property w: int;\n"
    "  public constructor(x: T, y: int)\n"
    "    v := x;\n"
    "    w := y;\n"
    "  end constructor;\n"
    "end Holder;\n"
    "record Pt(X: int, Y: int);\n"
    "class Box\n"
    "  public N: int;\n"
    "  public constructor(n: int)\n"
    "    N := n;\n"
    "  end constructor;\n"
    "end Box;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hp := new Holder<Pt>(new Pt(4, 5), 33);\n"
    "    var gp: () -> Pt := hp.v;\n"
    "    var p := gp();\n"
    "    var hb := new Holder<Box>(new Box(9), 44);\n"
    "    var gb: () -> Box := hb.v;\n"
    "    var gw: () -> int := hb.w;\n"
    "    var gpw: () -> int := hp.w;\n"
    "    Console.WriteLine(p.X * 10 + p.Y);\n"
    "    Console.WriteLine(gb().N);\n"
    "    Console.WriteLine(gpw());\n"
    "    Console.WriteLine(gw());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// e_three.bzl
constexpr const char* k_e_three =
    "module M;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public property w: int;\n"
    "  public constructor(x: T, y: int)\n"
    "    v := x;\n"
    "    w := y;\n"
    "  end constructor;\n"
    "end Holder;\n"
    "record Pt(X: int, Y: int);\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hi := new Holder<int>(3, 11);\n"
    "    var hs := new Holder<string>(\"z\", 22);\n"
    "    var hp := new Holder<Pt>(new Pt(4, 5), 33);\n"
    "    var gi: () -> int := hi.v;\n"
    "    var gs: () -> string := hs.v;\n"
    "    var gp: () -> Pt := hp.v;\n"
    "    var wi: () -> int := hi.w;\n"
    "    var ws: () -> int := hs.w;\n"
    "    var wp: () -> int := hp.w;\n"
    "    Console.WriteLine(gi());\n"
    "    Console.WriteLine(gs());\n"
    "    Console.WriteLine(gp().X * 10 + gp().Y);\n"
    "    Console.WriteLine(wi() + ws() + wp());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// f_genmethod.bzl
constexpr const char* k_f_genmethod =
    "module M;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public property w: int;\n"
    "  public constructor(x: T, y: int)\n"
    "    v := x;\n"
    "    w := y;\n"
    "  end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Read<U>(h: Holder<U>): U\n"
    "    var g: () -> U := h.v;\n"
    "    return g();\n"
    "  end Read;\n"
    "  public static method ReadW<U>(h: Holder<U>): int\n"
    "    var g: () -> int := h.w;\n"
    "    return g();\n"
    "  end ReadW;\n"
    "  public static method Main(): int\n"
    "    var hi := new Holder<int>(3, 11);\n"
    "    var hs := new Holder<string>(\"z\", 22);\n"
    "    Console.WriteLine(Read<int>(hi));\n"
    "    Console.WriteLine(Read<string>(hs));\n"
    "    Console.WriteLine(ReadW<int>(hi));\n"
    "    Console.WriteLine(ReadW<string>(hs));\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// h_fn_int.bzl
constexpr const char* k_h_fn_int =
    "module M;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public property w: int;\n"
    "  public constructor(x: T, y: int)\n"
    "    v := x;\n"
    "    w := y;\n"
    "  end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Read<U>(h: Holder<U>): U\n"
    "    var g: () -> U := h.v;\n"
    "    return g();\n"
    "  end Read;\n"
    "  public static method Main(): int\n"
    "    var hi := new Holder<int>(3, 11);\n"
    "    var hf := new Holder<() -> int>(lambda () => 7, 22);\n"
    "    var gi: () -> int := hi.v;\n"
    "    var gf: () -> int := hf.v;\n"
    "    Console.WriteLine(gi());\n"
    "    Console.WriteLine(gf());\n"
    "    Console.WriteLine(Read<int>(hi));\n"
    "    var f := Read<() -> int>(hf);\n"
    "    Console.WriteLine(f());\n"
    "    var wi: () -> int := hi.w;\n"
    "    var wf: () -> int := hf.w;\n"
    "    Console.WriteLine(wi() + wf());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// i_fn_fn.bzl
constexpr const char* k_i_fn_fn =
    "module M;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public property w: int;\n"
    "  public constructor(x: T, y: int)\n"
    "    v := x;\n"
    "    w := y;\n"
    "  end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Read<U>(h: Holder<U>): U\n"
    "    var g: () -> U := h.v;\n"
    "    return g();\n"
    "  end Read;\n"
    "  public static method Main(): int\n"
    "    var hf := new Holder<() -> int>(lambda () => 7, 22);\n"
    "    var hs := new Holder<() -> string>(lambda () => \"q\", 33);\n"
    "    var gf: () -> int := hf.v;\n"
    "    var gs: () -> string := hs.v;\n"
    "    Console.WriteLine(gf());\n"
    "    Console.WriteLine(gs());\n"
    "    var f := Read<() -> int>(hf);\n"
    "    var s := Read<() -> string>(hs);\n"
    "    Console.WriteLine(f());\n"
    "    Console.WriteLine(s());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// g_direct.bzl
constexpr const char* k_g_direct =
    "module M;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public property w: int;\n"
    "  public constructor(x: T, y: int)\n"
    "    v := x;\n"
    "    w := y;\n"
    "  end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hi := new Holder<int>(3, 11);\n"
    "    var hs := new Holder<string>(\"z\", 22);\n"
    "    Console.WriteLine(hi.v);\n"
    "    Console.WriteLine(hs.v);\n"
    "    Console.WriteLine(hi.w + hs.w);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// n_this_in_class.bzl
constexpr const char* k_n_this_in_class =
    "module M;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public property w: int;\n"
    "  public constructor(x: T, y: int)\n"
    "    v := x;\n"
    "    w := y;\n"
    "  end constructor;\n"
    "  public method GetV(): T\n"
    "    var g: () -> T := this.v;\n"
    "    return g();\n"
    "  end GetV;\n"
    "  public method GetW(): int\n"
    "    var g: () -> int := this.w;\n"
    "    return g();\n"
    "  end GetW;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hi := new Holder<int>(3, 11);\n"
    "    var hs := new Holder<string>(\"z\", 22);\n"
    "    Console.WriteLine(hi.GetV());\n"
    "    Console.WriteLine(hs.GetV());\n"
    "    Console.WriteLine(hi.GetW());\n"
    "    Console.WriteLine(hs.GetW());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// o_equality.bzl
constexpr const char* k_o_equality =
    "module M;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public constructor(x: T)\n"
    "    v := x;\n"
    "  end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hi := new Holder<int>(3);\n"
    "    var hs := new Holder<string>(\"z\");\n"
    "    var a1: () -> int := hi.v;\n"
    "    var a2: () -> int := hi.v;\n"
    "    var b1: () -> string := hs.v;\n"
    "    var b2: () -> string := hs.v;\n"
    "    Console.WriteLine(a1 = a2);\n"
    "    Console.WriteLine(b1 = b2);\n"
    "    Console.WriteLine(a1() + 1);\n"
    "    Console.WriteLine(b1() + b2());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// fix-forward #5306 (review R-1): the same property at the same instantiation,
// referenced from THREE sites — `this.v` inside a generic METHOD of the
// generic class at two different method type arguments, and `hi.v` in `Main`
// — must yield one thunk (§27.7.1, v0.313: "property references follow the
// method-reference rule", so the three are "guaranteed equal"). 200 rounds,
// both an `int` and a `string` T; the counters print once.
// k200_this_sites.bzl
constexpr const char* k_k_this_sites =
    "module M;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public constructor(t: T) v := t; end constructor;\n"
    "  public method RefAt<U>(u: U): () -> T\n"
    "    return this.v;\n"
    "  end RefAt;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hi := new Holder<int>(3);\n"
    "    var hs := new Holder<string>(\"z\");\n"
    "    var eqi := 0;\n"
    "    var eqs := 0;\n"
    "    var sum := 0;\n"
    "    var zs := 0;\n"
    "    for var i in 1..200 do\n"
    "      var r1 := hi.RefAt<int>(1);\n"
    "      var r2 := hi.RefAt<string>(\"u\");\n"
    "      var r0: () -> int := hi.v;\n"
    "      if r1 = r2 and r1 = r0 and r2 = r0 then\n"
    "        eqi := eqi + 1;\n"
    "      end if;\n"
    "      sum := sum + r1() + r2() + r0();\n"
    "      var s1 := hs.RefAt<int>(1);\n"
    "      var s2 := hs.RefAt<string>(\"u\");\n"
    "      var s0: () -> string := hs.v;\n"
    "      if s1 = s2 and s1 = s0 and s2 = s0 then\n"
    "        eqs := eqs + 1;\n"
    "      end if;\n"
    "      if s1() = \"z\" and s2() = \"z\" and s0() = \"z\" then\n"
    "        zs := zs + 1;\n"
    "      end if;\n"
    "    end for;\n"
    "    Console.WriteLine(eqi);\n"
    "    Console.WriteLine(eqs);\n"
    "    Console.WriteLine(sum);\n"
    "    Console.WriteLine(zs);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The bare-name (implicit `this`) spelling of the same program: `return v;`.
// m200_bare_sites.bzl
constexpr const char* k_m_bare_sites =
    "module M;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public constructor(t: T) v := t; end constructor;\n"
    "  public method RefAt<U>(u: U): () -> T\n"
    "    return v;\n"
    "  end RefAt;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hi := new Holder<int>(3);\n"
    "    var hs := new Holder<string>(\"z\");\n"
    "    var eqi := 0;\n"
    "    var eqs := 0;\n"
    "    var sum := 0;\n"
    "    var zs := 0;\n"
    "    for var i in 1..200 do\n"
    "      var r1 := hi.RefAt<int>(1);\n"
    "      var r2 := hi.RefAt<string>(\"u\");\n"
    "      var r0: () -> int := hi.v;\n"
    "      if r1 = r2 and r1 = r0 and r2 = r0 then\n"
    "        eqi := eqi + 1;\n"
    "      end if;\n"
    "      sum := sum + r1() + r2() + r0();\n"
    "      var s1 := hs.RefAt<int>(1);\n"
    "      var s2 := hs.RefAt<string>(\"u\");\n"
    "      var s0: () -> string := hs.v;\n"
    "      if s1 = s2 and s1 = s0 and s2 = s0 then\n"
    "        eqs := eqs + 1;\n"
    "      end if;\n"
    "      if s1() = \"z\" and s2() = \"z\" and s0() = \"z\" then\n"
    "        zs := zs + 1;\n"
    "      end if;\n"
    "    end for;\n"
    "    Console.WriteLine(eqi);\n"
    "    Console.WriteLine(eqs);\n"
    "    Console.WriteLine(sum);\n"
    "    Console.WriteLine(zs);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The review's step-2 programs (a, b, c, d, e, g, h, i): shapes the
// per-instantiation key must keep telling apart, or keep together.
// a_nested.bzl: a NESTED generic argument — `Holder<Box<int>>` and
// `Holder<Box<string>>` both lower `T` to `ptr`; only the identity half of
// the key separates them.
constexpr const char* k_p_nested =
    "module M;\n"
    "class Box<E>\n"
    "  public property x: E;\n"
    "  public constructor(e: E) x := e; end constructor;\n"
    "end Box;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public property w: int;\n"
    "  public constructor(t: T, n: int) v := t; w := n; end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hi := new Holder<Box<int>>(new Box<int>(5), 1);\n"
    "    var hs := new Holder<Box<string>>(new Box<string>(\"q\"), 2);\n"
    "    var gi: () -> Box<int> := hi.v;\n"
    "    var gs: () -> Box<string> := hs.v;\n"
    "    var wi: () -> int := hi.w;\n"
    "    var ws: () -> int := hs.w;\n"
    "    Console.WriteLine(gi().x);\n"
    "    Console.WriteLine(gs().x);\n"
    "    Console.WriteLine(wi());\n"
    "    Console.WriteLine(ws());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// b_eqbody.bzl: the SAME LLVM signature (`i1 (ptr)`) with a DIFFERENT body —
// `_a = _b` is string equality at `Pair<string>` and reference equality at
// `Pair<Obj>`.
constexpr const char* k_q_eqbody =
    "module M;\n"
    "class Obj\n"
    "  public property n: int;\n"
    "  public constructor(k: int) n := k; end constructor;\n"
    "end Obj;\n"
    "class Pair<T>\n"
    "  private _a: T;\n"
    "  private _b: T;\n"
    "  public constructor(a: T, b: T) _a := a; _b := b; end constructor;\n"
    "  public property Same: bool get => _a = _b; end;\n"
    "end Pair;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var s1 := \"ab\";\n"
    "    var s2 := \"a\" + \"b\";\n"
    "    var ps := new Pair<string>(s1, s2);\n"
    "    var o1 := new Obj(1);\n"
    "    var o2 := new Obj(1);\n"
    "    var po := new Pair<Obj>(o1, o2);\n"
    "    var fs: () -> bool := ps.Same;\n"
    "    var fo: () -> bool := po.Same;\n"
    "    Console.WriteLine(fs());\n"
    "    Console.WriteLine(fo());\n"
    "    Console.WriteLine(ps.Same);\n"
    "    Console.WriteLine(po.Same);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// c_genmethod_in_genclass.bzl: a generic method of the generic class forming
// `this.v` both as a VALUE read through a local and as a RETURNED reference,
// at two instantiations of the class and two of the method.
constexpr const char* k_r_genmethod_genclass =
    "module M;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public constructor(t: T) v := t; end constructor;\n"
    "  public method Pick<U>(u: U): T\n"
    "    var g: () -> T := this.v;\n"
    "    return g();\n"
    "  end Pick;\n"
    "  public method RefAt<U>(u: U): () -> T\n"
    "    return this.v;\n"
    "  end RefAt;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hi := new Holder<int>(3);\n"
    "    var hs := new Holder<string>(\"z\");\n"
    "    Console.WriteLine(hi.Pick<int>(1));\n"
    "    Console.WriteLine(hi.Pick<string>(\"u\"));\n"
    "    Console.WriteLine(hs.Pick<int>(1));\n"
    "    Console.WriteLine(hs.Pick<string>(\"u\"));\n"
    "    var r1 := hi.RefAt<int>(1);\n"
    "    var r2 := hi.RefAt<string>(\"u\");\n"
    "    var r0: () -> int := hi.v;\n"
    "    Console.WriteLine(r1 = r2);\n"
    "    Console.WriteLine(r1 = r0);\n"
    "    Console.WriteLine(r1() + r2());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// d_alias.bzl: receivers spelled through a plain alias, a second plain alias
// and a GENERIC alias (§27.8) resolve to the aliased instantiation.
constexpr const char* k_s_alias =
    "module M;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public constructor(t: T) v := t; end constructor;\n"
    "end Holder;\n"
    "type IH := Holder<int>;\n"
    "type SH := Holder<string>;\n"
    "type H<X> := Holder<X>;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var a: IH := new Holder<int>(3);\n"
    "    var b: SH := new Holder<string>(\"z\");\n"
    "    var c: H<string> := new Holder<string>(\"y\");\n"
    "    var ga: () -> int := a.v;\n"
    "    var gb: () -> string := b.v;\n"
    "    var gc: () -> string := c.v;\n"
    "    Console.WriteLine(ga());\n"
    "    Console.WriteLine(gb());\n"
    "    Console.WriteLine(gc());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// e_iface.bzl: an INTERFACE-typed receiver (`IHas<int>` / `IHas<string>`); the
// property symbol's owner is the generic interface.
constexpr const char* k_t_iface =
    "module M;\n"
    "public interface IHas<T>\n"
    "  property v: T;\n"
    "end IHas;\n"
    "public class Holder<T>: IHas<T>\n"
    "  public constructor(t: T) v := t; end constructor;\n"
    "  public property v: T;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Main(): int\n"
    "    var hi := new Holder<int>(3);\n"
    "    var hs := new Holder<string>(\"z\");\n"
    "    var ii: IHas<int> := hi;\n"
    "    var js: IHas<string> := hs;\n"
    "    var gi: () -> int := ii.v;\n"
    "    var gs: () -> string := js.v;\n"
    "    Console.WriteLine(gi());\n"
    "    Console.WriteLine(gs());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// g_closure.bzl: the reference formed inside a generic STATIC method
// (`Make<U>(h: Holder<U>)`, receiver `Holder<U>` at `U = int`) is the same
// thunk as `hi.v` formed in `Main` at `Holder<int>` — the key must identify
// the OWNER's instantiation, not the enclosing method's parameter.
constexpr const char* k_u_closure =
    "module M;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public constructor(t: T) v := t; end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Make<U>(h: Holder<U>): () -> U\n"
    "    return h.v;\n"
    "  end Make;\n"
    "  public static method Main(): int\n"
    "    var hi := new Holder<int>(3);\n"
    "    var hs := new Holder<string>(\"z\");\n"
    "    var fi := Make<int>(hi);\n"
    "    var fs := Make<string>(hs);\n"
    "    var li := lambda () => fi() + 1;\n"
    "    var ls := lambda () => fs() + \"!\";\n"
    "    Console.WriteLine(li());\n"
    "    Console.WriteLine(ls());\n"
    "    var d: () -> int := hi.v;\n"
    "    Console.WriteLine(fi = d);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// h_twosites.bzl: FOUR sites for one instantiation — `Main`, a non-generic
// static method, a generic static method, and the class's own `Self()` — all
// equal; and two sites for the `string` instantiation equal too.
constexpr const char* k_v_foursites =
    "module M;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public constructor(t: T) v := t; end constructor;\n"
    "  public method Self(): () -> T\n"
    "    return this.v;\n"
    "  end Self;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method RefI(h: Holder<int>): () -> int\n"
    "    return h.v;\n"
    "  end RefI;\n"
    "  public static method RefG<U>(h: Holder<U>): () -> U\n"
    "    return h.v;\n"
    "  end RefG;\n"
    "  public static method Main(): int\n"
    "    var hi := new Holder<int>(3);\n"
    "    var hs := new Holder<string>(\"z\");\n"
    "    var a: () -> int := hi.v;\n"
    "    var b := RefI(hi);\n"
    "    var c := RefG<int>(hi);\n"
    "    var d := hi.Self();\n"
    "    var e: () -> string := hs.v;\n"
    "    var f := RefG<string>(hs);\n"
    "    Console.WriteLine(a = b);\n"
    "    Console.WriteLine(a = c);\n"
    "    Console.WriteLine(a = d);\n"
    "    Console.WriteLine(e = f);\n"
    "    Console.WriteLine(a() + b() + c() + d());\n"
    "    Console.WriteLine(e() + f());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// i_nongeneric_split.bzl: a NON-generic owner's property, referenced inside a
// generic method at two type arguments and in `Main` — never split.
constexpr const char* k_w_nongeneric =
    "module M;\n"
    "class C\n"
    "  public property n: int;\n"
    "  public constructor(k: int) n := k; end constructor;\n"
    "end C;\n"
    "class Program\n"
    "  public static method G<U>(c: C, u: U): () -> int\n"
    "    return c.n;\n"
    "  end G;\n"
    "  public static method Main(): int\n"
    "    var c := new C(7);\n"
    "    var a := G<int>(c, 1);\n"
    "    var b := G<string>(c, \"s\");\n"
    "    var d: () -> int := c.n;\n"
    "    Console.WriteLine(a = b);\n"
    "    Console.WriteLine(a = d);\n"
    "    Console.WriteLine(a() + b() + d());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

}  // namespace

// #5306's own repro (plus the sibling `w` the other rows use): `Holder<int>`
// first, then `Holder<string>`, `v` read in function-value position at both.
// Measured on ef32951c: ONE `pthunk` (`i32 (ptr)`) served both and printed
// `3` then an empty line. Post-fix: two thunks (`BEZEL_DUMP_IR=1` shows
// `define internal i32 @"pthunk$0"` and `define internal ptr @"pthunk$1"`).
// Pre-#5283 (r23-c28) the program did not compile (IR verifier). Also run
// under runAotChecked: the per-instantiation thunk path is ASan/LSan-clean.
BEZEL_TEST(generic_property_thunk_per_instantiation_5306,
           issue_5306_repro_int_then_string) {
    JitResult r = runAot(k_a_int_str);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\nz\n"));
    JitResult c = runAotChecked(k_a_int_str);
    BEZEL_EXPECT_TRUE(c.ran);
    BEZEL_EXPECT_EQ(c.exitCode, 0);
    BEZEL_EXPECT_EQ(c.stdoutText, std::string("3\nz\n"));
}

// The reverse order. Measured on ef32951c: the `ptr` thunk built for
// `Holder<string>` served `Holder<int>` and printed `z` then `11` (it read 8
// bytes across `v` and the adjacent `w`).
BEZEL_TEST(generic_property_thunk_per_instantiation_5306,
           issue_5306_repro_string_then_int) {
    JitResult r = runAot(k_b_str_int);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("z\n3\n"));
}

// The BODY-differs case: `w: int` has the SAME thunk signature (`i32 (ptr)`)
// at every instantiation, so a memo keyed on the thunk's LLVM function type
// would still share it — but its byte offset moves (`class.Holder$spec0` =
// {ptr, i8, i32, i32} puts it at 12; `$spec1` = {ptr, i8, ptr, i32} at 24).
// A shared thunk reads offset 12 of a `Holder<string>` — the upper half of
// the `v` pointer. Measured on ef32951c: `11` then `4435280`.
BEZEL_TEST(generic_property_thunk_per_instantiation_5306,
           sibling_int_property_offset_moves_between_instantiations) {
    JitResult r = runAot(k_c_sibling);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("11\n22\n"));
}

// A RECORD `T` (by-value `{i32, i32}` field, an aggregate thunk result) and a
// CLASS `T` (a `ptr` field): different signatures AND different layouts, and
// the `w` sibling at both. Measured on ef32951c: `45`, then a crash (core
// dump) — two thunks were emitted there (`hp.v`'s `%record.Pt (ptr)` and
// `hb.w`'s), so `hb.v` reused the record-returning one and `hp.w` the
// `Holder<Box>`-layout `w` thunk.
BEZEL_TEST(generic_property_thunk_per_instantiation_5306,
           record_and_class_substitutions_have_distinct_thunks) {
    JitResult r = runAot(k_d_rec_cls);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("45\n9\n33\n44\n"));
}

// Three instantiations (`int`, `string`, record `Pt`), `v` and `w` at each,
// all six references formed before any is called. Post-fix: six distinct
// thunks (`BEZEL_DUMP_IR=1`); ef32951c emitted two. Measured on ef32951c: `3`, empty, `109371027`, `4443424`.
BEZEL_TEST(generic_property_thunk_per_instantiation_5306,
           three_instantiations_of_one_generic_property) {
    JitResult r = runAot(k_e_three);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\nz\n45\n66\n"));
}

// The reference formed INSIDE a generic method at `U`: the receiver is
// `Holder<U>`, so the substitution makePropertyThunk installs comes from the
// CALLER's own §14.1 substitution (`lowerTypeRef(U)`), once per instantiation
// of `Read`/`ReadW`. Measured on ef32951c: `3`, empty, `11`, `4439376`.
BEZEL_TEST(generic_property_thunk_per_instantiation_5306,
           generic_method_reads_property_at_its_own_type_parameter) {
    JitResult r = runAot(k_f_genmethod);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\nz\n11\n22\n"));
}

// `int` and `() -> int`. At the call site `hf.v` is an ordinary value read
// (#5283 defect 2's carve-out: the substituted type is a function type), but
// inside `Read<U>` the receiver is `Holder<U>` and the reference IS a §27.6
// getter reference — so at `U = () -> int` a `ptr`-returning thunk is built
// next to the `i32` one for `U = int`. Plus the `w` sibling at both layouts.
// Measured on ef32951c: `3`, `7`, `3`, then a crash (core dump).
BEZEL_TEST(generic_property_thunk_per_instantiation_5306,
           function_typed_and_int_instantiations) {
    JitResult r = runAot(k_h_fn_int);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n7\n3\n7\n33\n"));
}

// CONTROL for the identity half of the key: `() -> int` and `() -> string`,
// through the carve-out at the call site and through `Read<U>`'s thunk. Both
// substitutions lower to `ptr` and the two layouts coincide, so a shared
// thunk is behaviourally CORRECT here and this row cannot by itself pin
// distinctness; it pins that the new key does not break the shape.
// (MEASURED post-fix: `Read`'s two instantiations share ONE
// `Program$Read$spec` body — the method specialization itself is shared at
// equal LLVM signatures — so one `ptr` thunk is emitted for both, next to the
// erased template's `i32` one.) Measured on ef32951c: `7`, `q`, then a crash
// (core dump): the only thunk there was the erased template's `i32 (ptr)`.
BEZEL_TEST(generic_property_thunk_per_instantiation_5306,
           two_function_typed_instantiations) {
    JitResult r = runAot(k_i_fn_fn);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\nq\n7\nq\n"));
}

// CONTROL (passes on r23-c28, ef32951c and post-fix): the same two
// instantiations read DIRECTLY, never as a function value. No thunk is built
// (`BEZEL_DUMP_IR=1`: 0 `pthunk`s), so this pins that the fix did not touch
// the ordinary read path.
BEZEL_TEST(generic_property_thunk_per_instantiation_5306,
           direct_property_read_control) {
    JitResult r = runAot(k_g_direct);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\nz\n33\n"));
}

// The reference formed INSIDE the generic class's own method (`this.v`,
// `this.w`), emitted once per instantiation of `Holder<T>`. The receiver's
// sema type here is `Holder<T>` — ONE `sema::TypeRef` shared by every
// instantiation of the method body — so a memo keyed on the receiver
// expression's TypeRef pointer would still share one thunk across
// instantiations; only the installed substitution tells them apart.
// MEASURED: `3`, `z`, `11`, `22` post-fix; with the memo keyed on
// {symbol, receiver TypeRef*} instead (an alternative tried for #5306 on
// branch a26-5283-fresh, 72a9d93c) `3`, empty, `11`, `4439376`. Fails on
// ef32951c and on r23-c28 too (wrong stdout).
BEZEL_TEST(generic_property_thunk_per_instantiation_5306,
           generic_class_method_forms_this_property_reference) {
    JitResult r = runAot(k_n_this_in_class);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\nz\n11\n22\n"));
}

// CONTROL (§27.7.1, v0.313: "property references follow the method-reference
// rule"; #1544): two references to the SAME property on the SAME receiver at
// the SAME instantiation still compare EQUAL — the per-instantiation key must
// not split them — at both an `int` and a `string` instantiation of one
// generic property in one program. The equality half holds under ANY key
// that is stable within an instantiation (the pre-fix shared thunk was equal
// to itself too); the value half pins the per-instantiation thunks.
// MEASURED: the program crashes on ef32951c and on r23-c28 (`hs.v`'s
// references were handed `Holder<int>`'s thunk).
BEZEL_TEST(generic_property_thunk_per_instantiation_5306,
           same_instantiation_references_still_compare_equal) {
    JitResult r = runAot(k_o_equality);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nTrue\n4\nzz\n"));
}

// fix-forward #5306 (review R-1; §27.7.1 / §14.1, v0.313). `this.v` inside
// `RefAt<U>` of `Holder<T>`: `this` carries no exprType, so the §14.1 install
// block in makePropertyThunk does not run and the memo key was computed from
// the WHOLE live `genericSubst_` — which inside a specialized generic method
// also binds the METHOD's `U`. One property at one instantiation (`Holder<int>`)
// therefore got a thunk per `U` and per site, and `hi.RefAt<int>(1) =
// hi.RefAt<string>("u")`, `= hi.v` were all False: §27.7.1's "guaranteed
// equal" broken, and with it event `-=` unsubscription. The key is now the
// OWNER's parameters only (`genericParamNames(ps->parent)`), so all three
// sites share `Holder<int>`'s thunk and all three share `Holder<string>`'s.
// 200 rounds; the three counters must all read 200.
// FAIL-WITHOUT (MEASURED on 818e3ac5, the #5306 fix before this one): the
// two equality counters print `0` and `0` (values `1800` / `200` correct).
// Passes on r23-c28 and on #5283 alone (one thunk per property, trivially
// equal — and wrong across instantiations, which the rows above pin).
BEZEL_TEST(generic_property_thunk_per_instantiation_5306,
           this_reference_inside_generic_method_equals_caller_reference_27_7_1) {
    JitResult r = runAot(k_k_this_sites);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200\n200\n1800\n200\n"));
}

// The bare-name (implicit `this`) spelling: `return v;`. Here `ma` is null
// (an IdentifierExpr, not a MemberAccessExpr), so the install block cannot
// run either and the same whole-map key split the thunk per `U`. Same
// expectation and the same FAIL-WITHOUT shape as the row above (`0`, `0`,
// `1800`, `200` on 818e3ac5). NOTE: the bare-name reference leaks its closure
// cell per evaluation on r23-c28 already (#5318, non-generic, independent of
// this memo); this row therefore stays on plain `runAot`, whose sanitized leg
// runs with `detect_leaks=0`, and must not gain a `runAotChecked` leg until
// #5318 lands.
BEZEL_TEST(generic_property_thunk_per_instantiation_5306,
           bare_name_reference_inside_generic_method_equals_caller_reference_27_7_1) {
    JitResult r = runAot(k_m_bare_sites);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200\n200\n1800\n200\n"));
}

// A NESTED generic argument reached through a generic method:
// `Read<U>(h: Holder<Box<U>>)` at `U = int` forms `h.v` on a `Holder<Box<int>>`,
// and that reference must equal `hi.v` formed in `Main`. Here the §14.1
// install block DOES run, but it resolved the receiver's argument shallowly
// (`resolveTypeParamRef`: a bare `U` only), so the key carried `Box<U>` — a
// name bound in the ENCLOSING method — instead of the owner's instantiation
// `Box<int>`. The argument is now composed through the outer map
// (`substituteTypeRefLocal`, as the method-call path does, #2192).
// FAIL-WITHOUT (MEASURED on 818e3ac5): `5`, `q`, `False` — values right,
// equality wrong.
// x4_nested_through_generic.bzl
constexpr const char* k_x_nested_via_generic =
    "module M;\n"
    "class Box<E>\n"
    "  public property x: E;\n"
    "  public constructor(e: E) x := e; end constructor;\n"
    "end Box;\n"
    "class Holder<T>\n"
    "  public property v: T;\n"
    "  public constructor(t: T) v := t; end constructor;\n"
    "end Holder;\n"
    "class Program\n"
    "  public static method Read<U>(h: Holder<Box<U>>): () -> Box<U>\n"
    "    return h.v;\n"
    "  end Read;\n"
    "  public static method Main(): int\n"
    "    var hi := new Holder<Box<int>>(new Box<int>(5));\n"
    "    var hs := new Holder<Box<string>>(new Box<string>(\"q\"));\n"
    "    var fi := Read<int>(hi);\n"
    "    var fs := Read<string>(hs);\n"
    "    var di: () -> Box<int> := hi.v;\n"
    "    Console.WriteLine(fi().x);\n"
    "    Console.WriteLine(fs().x);\n"
    "    Console.WriteLine(fi = di);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

BEZEL_TEST(generic_property_thunk_per_instantiation_5306,
           nested_argument_reference_via_generic_method_equals_caller_reference) {
    JitResult r = runAot(k_x_nested_via_generic);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\nq\nTrue\n"));
}

// ---- The review's step-2 shapes. (c) is a third pin of R-1 (it compares
// ---- `RefAt<int>` with `RefAt<string>`; FAIL-WITHOUT on 818e3ac5: `3`, `3`,
// ---- `z`, `z`, `False`, `False`, `6`). The rest are CONTROLS for the
// ---- owner-only key: each passes on 818e3ac5 and must keep passing;
// ---- together they pin that narrowing the key to the owner's parameters
// ---- neither merges instantiations that differ (a, b, d, e) nor splits
// ---- references that must be equal (g, h, i).

// (a) Nested generic argument: `Holder<Box<int>>` vs `Holder<Box<string>>`.
// Both `T`s lower to `ptr`; only the identity half of the key separates them.
BEZEL_TEST(generic_property_thunk_per_instantiation_5306,
           nested_generic_argument_instantiations_have_distinct_thunks) {
    JitResult r = runAot(k_p_nested);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\nq\n1\n2\n"));
}

// (b) Same LLVM signature, different BODY: `_a = _b` is string equality at
// `Pair<string>` (True for two equal strings) and reference equality at
// `Pair<Obj>` (False for two distinct objects).
BEZEL_TEST(generic_property_thunk_per_instantiation_5306,
           same_signature_different_body_string_vs_class_equality_getter) {
    JitResult r = runAot(k_q_eqbody);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nFalse\nTrue\nFalse\n"));
}

// (c) A generic method of the generic class forming `this.v` as a value read
// and as a returned reference, at two class and two method instantiations;
// the returned references at `U = int` and `U = string` must be equal (R-1).
BEZEL_TEST(generic_property_thunk_per_instantiation_5306,
           generic_method_of_generic_class_value_reads_and_references) {
    JitResult r = runAot(k_r_genmethod_genclass);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("3\n3\nz\nz\nTrue\nTrue\n6\n"));
}

// (d) Alias-spelled receivers (§27.8): two plain aliases and a generic alias.
BEZEL_TEST(generic_property_thunk_per_instantiation_5306,
           alias_spelled_receivers_resolve_to_the_aliased_instantiation) {
    JitResult r = runAot(k_s_alias);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\nz\ny\n"));
}

// (e) Interface-typed receivers: the owner is the generic INTERFACE.
BEZEL_TEST(generic_property_thunk_per_instantiation_5306,
           interface_typed_receiver_instantiations_have_distinct_thunks) {
    JitResult r = runAot(k_t_iface);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\nz\n"));
}

// (g) A reference formed inside `Make<U>(h: Holder<U>)` at `U = int` equals
// `hi.v` formed in `Main` — the key identifies `Holder<int>`, not `U`.
BEZEL_TEST(generic_property_thunk_per_instantiation_5306,
           reference_formed_in_generic_static_method_equals_caller_reference) {
    JitResult r = runAot(k_u_closure);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\nz!\nTrue\n"));
}

// (h) Four sites, one instantiation, all equal (and two sites for `string`).
BEZEL_TEST(generic_property_thunk_per_instantiation_5306,
           four_sites_one_instantiation_all_compare_equal) {
    JitResult r = runAot(k_v_foursites);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("True\nTrue\nTrue\nTrue\n12\nzz\n"));
}

// (i) A NON-generic owner is never split by the enclosing method's `U`.
BEZEL_TEST(generic_property_thunk_per_instantiation_5306,
           non_generic_owner_not_split_by_enclosing_method_substitution) {
    JitResult r = runAot(k_w_nongeneric);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nTrue\n21\n"));
}
