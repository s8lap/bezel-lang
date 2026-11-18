//! case: s28_3_7635
//! doc: spec
//! section: 28.3
//! title: a lambda may not reassign a captured owned array local
//! rule: a closure cannot consume, reassign, or dispose a captured owned binding
//! cites: 28.3, 28.9
//! expect: reject
//! diagnostic: cannot assign to captured owned binding
module Conf;
import System;
public class P
  static method One(n: int): int
    var a: Array<int> := [1, n, 3];
    var f := lambda(): int => do a := [7, 8]; return a.Count; end;
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
