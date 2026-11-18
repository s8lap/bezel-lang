//! case: s28_9_7609
//! doc: spec
//! section: 28.9
//! title: a function-typed local bound to an Object local stays valid for the loop iteration
//! rule: Runtime management governs the *function value itself*
//! cites: 8.1, 28.9
//! expect: run
//! stdout: 200
module Conf;
import System;
public class Program
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      var f := lambda() => 5;
      var g: Object := f;
      t := t + 1;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
