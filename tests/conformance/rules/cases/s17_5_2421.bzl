//! case: s17_5_2421
//! doc: spec
//! section: 17.5
//! title: `in` over an IReadOnlyList<string> is the hashable Contains of
//!   IEnumerable<T>
//! rule: any accessible method named `Contains` taking one parameter of compatible
//!   type and returning `bool` qualifies, whether it is an instance member or
//!   an extension method
//! cites: 17.5
//! expect: run
//! output: True
//!   False
//! issue: 8260
//! notes: Red on base: the saved pre-fix compiler of the #8260 review (b77fbf69's
//!   parent) stopped at 'not yet implemented: codegen for this `in` expression
//!   form' on every `in` over an interface-typed collection, so the program did
//!   not compile. IReadOnlyList declares no Contains of its own, so the
//!   structural lookup of 17.5 reaches the extension Contains of stdlib 5 on
//!   IEnumerable<T> where T: hashable (string is hashable).
module P;

class C
  public static method Main(): int
    var arr := new Array<string>();
    arr.Add("q");
    var r: IReadOnlyList<string> := arr;
    Console.WriteLine("q" in r);
    Console.WriteLine("z" in r);
    return 0;
  end Main;
end C;
