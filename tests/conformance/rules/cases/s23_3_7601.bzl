//! case: s23_3_7601
//! doc: spec
//! section: 23.3
//! title: a setter that assigns another property inside its body keeps each setter value separate
//! rule: The implicit setter parameter is `value`. It is a parameter of the setter
//! cites: 27.5
//! expect: run
//! stdout: 60100
module Conf;
import System;
public class H
  public n: int;
  public m: int;
  public property Q: int
    get => m;
    set => do var g := lambda(): int => value * 2; m := g(); end;
  end;
  public property P: int
    get => n;
    set => do
      var g := lambda(): int => value + 1;
      Q := g();
      var g2 := lambda(): int => value;
      n := g2();
    end;
  end;
end H;
public class Program
  public static method Main()
    var i := 0;
    var tot := 0;
    using var h := new H();
    while i < 200 do
      h.P := i;
      tot := tot + h.P + h.Q;
      i := i + 1;
    end;
    Console.WriteLine(tot);
  end Main;
end Program;
