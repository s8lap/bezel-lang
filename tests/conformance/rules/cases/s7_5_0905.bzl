//! case: s7_5_0905
//! doc: spec
//! section: 7.5
//! title: ?. on a null JsonOptions? yields null, which ?? replaces; the result also assigns to int?
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//! cites: 2.15, 7.2, 7.7
//! expect: run
//! output: -1
//!   True
//!   9
//!   -2
//! issue: 7966
module Conf;

import System.Serialization;

public class Program
  public static method Main()
    var o: JsonOptions? := new JsonOptions(true, 4, false, true, 9);
    var n: JsonOptions? := null;
    Console.WriteLine(n?.MaxDepth ?? -1);
    Console.WriteLine(n?.Indented ?? true);
    var x: int? := o?.MaxDepth;
    Console.WriteLine(x ?? -2);
    var y: int? := n?.MaxDepth;
    Console.WriteLine(y ?? -2);
  end Main;
end Program;
