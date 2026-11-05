//! case: s7_4_2425
//! doc: spec
//! section: 7.4
//! title: `7 in b` after the early exit `if b = null then return false`
//! rule: an early exit: after `if x = null then return; end;`
//! cites: 7.4, 17.5
//! expect: run
//! output: True
//!   False
//! issue: 8260
//! notes: Red on base: the saved pre-fix compiler of the #8260 review (b77fbf69's
//!   parent) stopped at 'not yet implemented: codegen for this `in` expression
//!   form' on every `in` over an interface-typed collection, so the program did
//!   not compile.
module P;

class C
  public static method Q(b: IList<int>?): bool
    if b = null then return false; end if;
    return 7 in b;
  end Q;
  public static method Main(): int
    var a := new Array<int>();
    a.Add(7);
    Console.WriteLine(Q(a));
    Console.WriteLine(Q(null));
    return 0;
  end Main;
end C;
