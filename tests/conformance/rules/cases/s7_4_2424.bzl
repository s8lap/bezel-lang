//! case: s7_4_2424
//! doc: spec
//! section: 7.4
//! title: `7 in b` after `if b <> null` over a nullable IList parameter
//! rule: a null test in either spelling
//! cites: 7.4, 17.5
//! expect: run
//! output: True
//!   False
//! issue: 8260
//! notes: Red on base: the saved pre-fix compiler of the #8260 review (b77fbf69's
//!   parent) stopped at 'not yet implemented: codegen for this `in` expression
//!   form' on every `in` over an interface-typed collection, so the program did
//!   not compile. Q(a) finds 7 and Q(null) takes the false path.
module P;

class C
  public static method Q(b: IList<int>?): bool
    if b <> null then
      return 7 in b;
    end if;
    return false;
  end Q;
  public static method Main(): int
    var a := new Array<int>();
    a.Add(7);
    Console.WriteLine(Q(a));
    Console.WriteLine(Q(null));
    return 0;
  end Main;
end C;
