//! case: s7_5_0906
//! doc: spec
//! section: 7.5
//! title: ?. evaluates the receiver once and skips the access when the receiver is null
//! rule: `?.` short-circuits. The receiver is evaluated first; if it is null the access does not occur
//! cites: 2.15, 7.7
//! expect: run
//! output: 7
//!   1
//!   -1
//!   2
//! issue: 7966
//! notes: Single evaluation of the receiver is observed through the ref counter.
module Conf;

import System.Serialization;

public class Program
  public static method Get(ref c: int, b: bool): JsonOptions?
    c := c + 1;
    if b then
      return new JsonOptions(true, 2, false, false, 7);
    end if;
    return null;
  end Get;

  public static method Main()
    var c := 0;
    var r := Get(ref c, true)?.MaxDepth ?? -1;
    Console.WriteLine(r);
    Console.WriteLine(c);
    var r2 := Get(ref c, false)?.MaxDepth ?? -1;
    Console.WriteLine(r2);
    Console.WriteLine(c);
  end Main;
end Program;
