//! case: s27_5_8720
//! doc: spec
//! section: 27.5
//! title: an imported virtual indexer read with a lambda index argument that captures the receiver
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5, 23.4, 2.8
//! expect: run
//! stdout: 400
//! notes: Pins #9099 (imported getter). The class lives in an imported module; the lambda index
//!   argument captures the receiver. q() + Tag = 1 + 1 = 2, 200 times: 400.
module Conf;
import LibA;
public class Program
  public static method Main(): int
    var t := 0;
    var fl := new LibA.FlA();
    for var j in 0 ..< 200 do
      t := t + fl[lambda() => fl.Tag];
    end for;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;
