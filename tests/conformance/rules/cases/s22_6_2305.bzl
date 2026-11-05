//! case: s22_6_2305
//! doc: spec
//! section: 22.6
//! title: a lambda returning an Array<string> parameter at an
//!   IEnumerable<string> slot iterates the strings
//! rule: the candidate is applicable at the position when the lambda's
//!   inferred return type equals the candidate's return slot, implicitly
//!   converts to it
//! cites: 5.1, 27.3
//! expect: run
//! output: 10
//! issue: 8289
//! notes: Red on base: the pre-fix compiler handed the raw collection
//!   pointer on as the IEnumerable and the caller's for-in crashed
//!   (segmentation fault), the staging tip converts and iterates.
module Conf;

public class H
  public static method Run(xs: Array<string>, f: (Array<string>) -> IEnumerable<string>): int
    var n := 0;
    for var v in f(xs) do n := n + v.Length; end;
    for var v in f(xs) do n := n + v.Length; end;
    return n;
  end Run;
end H;

public class Program
  public static method Main()
    var xs: Array<string> := ["ab", "cde"];
    Console.WriteLine(H.Run(xs, lambda(l) => l));
  end Main;
end Program;
