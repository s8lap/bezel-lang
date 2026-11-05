//! case: s17_5_2424
//! doc: spec
//! section: 17.5
//! title: A long left operand is rejected over IList<int> (no implicit narrowing)
//! rule: the left operand is admitted exactly when it is admissible as that call's
//!   argument under the ordinary conversion rules
//! cites: 17.5, 5.1
//! expect: reject
//! diagnostic: implicitly convertible
//! notes: Single ground: a long variable does not implicitly convert to int (5.1 has
//!   no narrowing conversion), so it is not admissible as Contains's argument.
//!   The accepting twin is s17_5_2422 in the widening direction. Removing the
//!   `in` expression leaves a compiling file. Not red on base by verdict: the
//!   saved pre-fix compiler also rejected it, with the 'not yet implemented'
//!   text, so only the diagnostic needle (reported, never gated) differs.
module P;

class C
  public static method Main(): int
    var a := new Array<int>();
    var l: IList<int> := a;
    var x: long := 3L;
    Console.WriteLine(x in l);
    return 0;
  end Main;
end C;
