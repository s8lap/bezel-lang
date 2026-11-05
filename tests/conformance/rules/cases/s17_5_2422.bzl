//! case: s17_5_2422
//! doc: spec
//! section: 17.5
//! title: An int literal is admitted as the left operand of `in` over IList<long>
//! rule: the left operand is admitted exactly when it is admissible as that call's
//!   argument under the ordinary conversion rules
//! cites: 17.5, 5.1
//! expect: run
//! output: True
//! issue: 8260
//! notes: Red on base: the saved pre-fix compiler of the #8260 review (b77fbf69's
//!   parent) stopped at 'not yet implemented: codegen for this `in` expression
//!   form' on every `in` over an interface-typed collection, so the program did
//!   not compile. The argument 3 converts to long under 5.1, so the call
//!   l.Contains(3) is admissible. (The double half of the review probe p27 is
//!   not taken: IReadOnlyList<double> reaches the hashable extension and the
//!   spec does not make double hashable.)
module P;

class C
  public static method Main(): int
    var a := new Array<long>();
    a.Add(3L);
    var l: IList<long> := a;
    Console.WriteLine(3 in l);
    return 0;
  end Main;
end C;
