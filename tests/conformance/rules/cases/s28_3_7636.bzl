//! case: s28_3_7636
//! doc: spec
//! section: 28.3
//! title: an enclosing body may reassign an owned array local that a lambda only reads
//! rule: a closure cannot consume, reassign, or dispose a captured owned binding
//! cites: 28.3, 28.9
//! expect: run
//! stdout: 1800
module Conf;
import System;
public class P
  static method One(n: int): int
    var a: Array<int> := [1, n, 3];
    a := [7, 8];
    var f := lambda(): int => a.Count;
    var r := f();
    return r + a[0];
  end One;
  public static method Main(): int
    var s := 0;
    for var i in 1..200 do s := s + One(i); end;
    Console.WriteLine(s);
    return 0;
  end Main;
end P;
