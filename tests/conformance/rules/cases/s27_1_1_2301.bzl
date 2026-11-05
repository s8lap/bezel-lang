//! case: s27_1_1_2301
//! doc: spec
//! section: 27.1.1
//! title: a lambda initialising an IEnumerable-returning function local
//!   converts its pipeline body
//! rule: its body's inferred return type converts to the target's return
//!   slot under
//! cites: 5.1, 27.3
//! expect: run
//! output: 2
//!   3
//! issue: 8289
//! notes: Red on base: the pre-fix compiler handed the raw collection
//!   pointer on as the IEnumerable and the caller's for-in crashed
//!   (segmentation fault), the staging tip converts and iterates.
module Conf;

public class Program
  public static method Main()
    var xs: Array<int> := [1, 2, 3];
    var f: (Array<int>) -> IEnumerable<int> := lambda(l) => l.Where(lambda(x) => x > 1);
    for var v in f(xs) do Console.WriteLine(v); end;
  end Main;
end Program;
