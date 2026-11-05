//! case: s17_5_2426
//! doc: spec
//! section: 17.5
//! title: A null left operand is admitted over IList<string?> (nullable Contains
//!   parameter)
//! rule: a nullable left operand is **admitted**
//! cites: 17.5, 8.4
//! expect: run
//! output: True
//! issue: 8260
//! notes: Red on base: the saved pre-fix compiler of the #8260 review (b77fbf69's
//!   parent) stopped at 'not yet implemented: codegen for this `in` expression
//!   form' on every `in` over an interface-typed collection, so the program did
//!   not compile. IList<string?>.Contains takes item: string?, so the nullable
//!   operand is admitted and the null-safe equality of 8.4 finds the stored
//!   null.
module P;

class C
  public static method Main(): int
    var a := new Array<string?>();
    a.Add(null);
    var l: IList<string?> := a;
    Console.WriteLine(null in l);
    return 0;
  end Main;
end C;
