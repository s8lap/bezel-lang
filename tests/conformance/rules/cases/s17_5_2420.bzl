//! case: s17_5_2420
//! doc: spec
//! section: 17.5
//! title: `not (i in l)` as a while condition over an IList local
//! rule: `x in collection` evaluates to `true` if `collection.Contains(x)` is true.
//! cites: 17.5
//! expect: run
//! output: 3
//! issue: 8260
//! notes: Red on base: the saved pre-fix compiler of the #8260 review (b77fbf69's
//!   parent) stopped at 'not yet implemented: codegen for this `in` expression
//!   form' on every `in` over an interface-typed collection, so the program did
//!   not compile. The loop stops at the first i for which l.Contains(i) is
//!   true, which is 3.
module P;

class C
  public static method Main(): int
    var arr := new Array<int>();
    arr.Add(3);
    var l: IList<int> := arr;
    var i := 0;
    while not (i in l) do i := i + 1; end while;
    Console.WriteLine(i);
    return 0;
  end Main;
end C;
