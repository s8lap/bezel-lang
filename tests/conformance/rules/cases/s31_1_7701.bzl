//! case: s31_1_7701
//! doc: spec
//! section: 31.1
//! title: throw move of a closure's own local is rejected at the marker-free position
//! rule: `throw move x` is rejected at the marker-free position
//! cites: 28.3
//! expect: reject
//! diagnostic: move
module Conf;
import System;
public class R: Exception
  public constructor() end constructor;
end R;
public class Program
  public static method Main(): int
    var f := lambda(): int => do var c := new R(); throw move c; end;
    try _ := f(); catch var x: R => Console.WriteLine("caught"); end;
    return 0;
  end Main;
end Program;
