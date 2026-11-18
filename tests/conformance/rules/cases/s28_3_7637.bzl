//! case: s28_3_7637
//! doc: spec
//! section: 28.3
//! title: a lambda may read a captured owned array local any number of times
//! rule: a closure cannot consume, reassign, or dispose a captured owned binding
//! cites: 28.3, 28.9
//! expect: run
//! stdout: 2200
module Conf;
import System;
public class P
  static method One(): int
    var a: Array<int> := [1, 2, 3];
    var f := lambda() => a[0] + a[2];
    return f() + f() + a.Count;
  end One;
  public static method Main(): int
    var s := 0;
    for var i in 1..200 do s := s + One(); end;
    Console.WriteLine(s);
    return 0;
  end Main;
end P;
