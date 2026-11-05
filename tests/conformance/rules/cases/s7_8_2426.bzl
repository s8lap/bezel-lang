//! case: s7_8_2426
//! doc: spec
//! section: 7.8
//! title: `7 in b!` asserts the nullable IList operand non-null
//! rule: The result is the non-nullable form of the expression.
//! cites: 7.8, 17.5
//! expect: run
//! output: True
//! issue: 8260
//! notes: Red on base: the saved pre-fix compiler of the #8260 review (b77fbf69's
//!   parent) stopped at 'not yet implemented: codegen for this `in` expression
//!   form' on every `in` over an interface-typed collection, so the program did
//!   not compile.
module P;

class C
  public static method Q(b: IList<int>?): bool
    return 7 in b!;
  end Q;
  public static method Main(): int
    var a := new Array<int>();
    a.Add(7);
    Console.WriteLine(Q(a));
    return 0;
  end Main;
end C;
