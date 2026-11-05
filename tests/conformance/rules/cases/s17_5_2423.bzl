//! case: s17_5_2423
//! doc: spec
//! section: 17.5
//! title: A string left operand is rejected over IList<int>
//! rule: the left operand is admitted exactly when it is admissible as that call's
//!   argument under the ordinary conversion rules
//! cites: 17.5, 5.1
//! expect: reject
//! diagnostic: implicitly convertible
//! notes: Single ground: a string is not admissible as the int argument of
//!   Contains(item: int) under the ordinary conversions of 5.1. The accepting
//!   twin replaces "a" with 3. Removing the `in` expression leaves a compiling
//!   file. Not red on base by verdict: the saved pre-fix compiler also rejected
//!   it, with the 'not yet implemented' text, so only the diagnostic needle
//!   (reported, never gated) differs.
module P;

class C
  public static method Main(): int
    var arr := new Array<int>();
    var l: IList<int> := arr;
    Console.WriteLine("a" in l);
    return 0;
  end Main;
end C;
