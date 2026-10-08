//! case: s15_1_0009
//! doc: spec
//! section: 15.1
//! title: an Array of an inline array is well-formed, indexed and iterated
//! rule: A standard-library declaration carries no requirement of this kind at
//!   its naming (§3.6 rule 2), so `Array<[3]float>` is a well-formed type,
//!   indexed and iterated as any other
//! cites: 15.1, 14.2
//! expect: run
//! output: 4
//!   9
module Conf;

public class Program
  public static method Main()
    using var xs := new Array<[3]int>();
    var a: [3]int := [1, 2, 3];
    xs.Add(a);
    var b: [3]int := [4, 5, 6];
    xs.Add(b);
    Console.WriteLine(xs[1][0]);
    var total := 0;
    for var row in xs do
      total := total + row[2];
    end for;
    Console.WriteLine(total);
  end Main;
end Program;
