//! case: s28_9_7610
//! doc: spec
//! section: 28.9
//! title: an Object alias keeps the original closure after the source local is reassigned
//! rule: Runtime management governs the *function value itself*
//! cites: 8.1, 28.9
//! expect: run
//! stdout: 400
module Conf;
import System;
public class Program
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      var f := lambda(x: int) => x + i;
      var g: Object := f;
      f := lambda(x: int) => x - i;
      var h := g as (int) -> int;
      t := t + h!(1) + f(1);
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
