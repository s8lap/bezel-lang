//! case: s17_5_2427
//! doc: spec
//! section: 17.5
//! title: Both operands of `in` are evaluated exactly once per evaluation of the
//!   expression
//! rule: `x in c` *is* the call `c.Contains(x)`
//! cites: 17.5
//! expect: run
//! output: 100
//!   100
//!   100
//! issue: 8260
//! notes: Red on base: the saved pre-fix compiler of the #8260 review (b77fbf69's
//!   parent) stopped at 'not yet implemented: codegen for this `in` expression
//!   form' on every `in` over an interface-typed collection, so the program did
//!   not compile. `X(c) in s.L(c)` is one call, so each of the two operand
//!   expressions runs once per iteration: hits, n and m are each 100 over 100
//!   iterations. The shape differs from the review probe p01, whose static L
//!   returned an interface (a plain-class return from a static method is an
//!   ownership error, CODE_GEN 4.3).
module P;

public class Ctr
  public n: int;
  public m: int;
  public constructor() n := 0; m := 0; end constructor;
end Ctr;

public class Src
  private owned _arr: Array<int> := new Array<int>();

  public constructor()
    _arr.Add(3);
  end constructor;

  public method L(c: Ctr): IList<int>
    c.m := c.m + 1;
    return _arr;
  end L;
end Src;

class C
  public static method X(c: Ctr): int
    c.n := c.n + 1;
    return 3;
  end X;
  public static method Main(): int
    using var s := new Src();
    var c := new Ctr();
    var hits := 0;
    for var i in 0..99 do
      if X(c) in s.L(c) then hits := hits + 1; end if;
    end for;
    Console.WriteLine(hits);
    Console.WriteLine(c.n);
    Console.WriteLine(c.m);
    return 0;
  end Main;
end C;
