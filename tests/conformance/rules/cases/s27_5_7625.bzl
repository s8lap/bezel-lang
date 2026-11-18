//! case: s27_5_7625
//! doc: spec
//! section: 27.5
//! title: an async lambda returning a nested lambda's read of a captured string gives the hand total
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 30.1
//! expect: run
//! stdout: 400
module Conf;
import System;
public class C
  public static async method RunS(t: string): Task<string>
    var s := t + "b";
    var f2 := async lambda() => do await Task.Delay(0); var g := lambda() => s.Length; return g(); end;
    var r := await f2();
    if r > 100 then s := s + "x"; end;
    return s;
  end RunS;
  public static async method Run(t: string): Task<int>
    var q := await C.RunS(t);
    return q.Length;
  end Run;
  public static async method Main(): Task<int>
    var total := 0;
    for var j in 0 ..< 200 do total := total + await C.Run("a"); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
