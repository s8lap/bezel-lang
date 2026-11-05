//! case: s7_4_2423
//! doc: spec
//! section: 7.4
//! title: A local declared nullable with a non-null initializer is narrowed at `in`
//! rule: from a declaration whose initializer is such an `expr`
//! cites: 7.4, 17.5
//! expect: run
//! output: True
//! issue: 8260
//! notes: Red on base: the saved pre-fix compiler of the #8260 review (b77fbf69's
//!   parent) stopped at 'not yet implemented: codegen for this `in` expression
//!   form' on every `in` over an interface-typed collection, so the program did
//!   not compile. The twin of s7_4_2422: the only edit is the initializer, `a`
//!   (an Array<int>, non-nullable) in place of `null`.
module P;

class C
  public static method Main(): int
    var a := new Array<int>();
    a.Add(7);
    var b: IList<int>? := a;
    Console.WriteLine(7 in b);
    return 0;
  end Main;
end C;
