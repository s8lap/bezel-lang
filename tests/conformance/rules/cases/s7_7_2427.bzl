//! case: s7_7_2427
//! doc: spec
//! section: 7.7
//! title: `7 in (b ?? d)` over a nullable IList operand and a non-nullable fallback
//! rule: `a ?? b` returns `a` if non-null, else `b`.
//! cites: 7.7, 17.5
//! expect: run
//! output: True
//!   False
//! issue: 8260
//! notes: Red on base: the saved pre-fix compiler of the #8260 review (b77fbf69's
//!   parent) stopped at 'not yet implemented: codegen for this `in` expression
//!   form' on every `in` over an interface-typed collection, so the program did
//!   not compile. Q(null, a) takes the fallback a, which holds 7; Q(e, a) takes
//!   e, which is empty.
module P;

class C
  public static method Q(b: IList<int>?, d: IList<int>): bool
    return 7 in (b ?? d);
  end Q;
  public static method Main(): int
    var a := new Array<int>();
    a.Add(7);
    var e := new Array<int>();
    Console.WriteLine(Q(null, a));
    Console.WriteLine(Q(e, a));
    return 0;
  end Main;
end C;
