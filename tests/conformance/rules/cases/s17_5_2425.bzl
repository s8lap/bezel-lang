//! case: s17_5_2425
//! doc: spec
//! section: 17.5
//! title: A null left operand over IList<string> is rejected (non-nullable Contains
//!   parameter)
//! rule: Where the selected `Contains` declares a non-nullable parameter, a
//!   nullable left operand is a **compile error**
//! cites: 17.5, 7.1
//! expect: reject
//! diagnostic: nullable
//! notes: Single ground: IList<string>.Contains takes item: string, a non-nullable
//!   parameter, so the nullable left operand `null` is a compile error. The
//!   accepting twin is the next case (IList<string?>). Removing the `in`
//!   expression leaves a compiling file. Not red on base by verdict: the saved
//!   pre-fix compiler also rejected it, with the 'not yet implemented' text, so
//!   only the diagnostic needle (reported, never gated) differs.
module P;

class C
  public static method Main(): int
    var m := new Array<string>();
    var k: IList<string> := m;
    Console.WriteLine(null in k);
    return 0;
  end Main;
end C;
