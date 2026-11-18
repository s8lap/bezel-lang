//! case: s27_5_7624
//! doc: spec
//! section: 27.5
//! title: a nested lambda inside an async lambda sees a later write to the captured string
//! rule: Modifications to a captured variable inside the lambda are visible outside, and vice versa
//! cites: 30.1
//! expect: run
//! stdout: 1200
module Conf;
import System;
public class C
  public static async method Run(t: string): Task<int>
    var s := t + "b";
    var f2 := async lambda() => do await Task.Delay(0); var g := lambda() => s.Length; s := s + "c"; return g(); end;
    var r := await f2();
    return r + s.Length;
  end Run;
  public static async method Main(): Task<int>
    var total := 0;
    for var j in 0 ..< 200 do total := total + await C.Run("a"); end;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
