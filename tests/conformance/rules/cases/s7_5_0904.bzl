//! case: s7_5_0904
//! doc: spec
//! section: 7.5
//! title: ?. reads each of the five JsonOptions components of a non-null JsonOptions? receiver
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//! cites: 2.15, 7.7
//! expect: run
//! output: 9
//!   4
//!   True
//!   False
//!   True
//! issue: 7966
module Conf;

import System.Serialization;

public class Program
  public static method Main()
    var o: JsonOptions? := new JsonOptions(true, 4, false, true, 9);
    Console.WriteLine(o?.MaxDepth ?? -1);
    Console.WriteLine(o?.IndentWidth ?? -1);
    Console.WriteLine(o?.Indented ?? false);
    Console.WriteLine(o?.AllowTrailingCommas ?? true);
    Console.WriteLine(o?.IgnoreUnknownFields ?? false);
  end Main;
end Program;
