//! case: s23_3_7600
//! doc: spec
//! section: 23.3
//! title: a setter whose body forms a lambda local and then throws leaves later calls correct
//! rule: The implicit setter parameter is `value`. It is a parameter of the setter
//! cites: 27.5
//! expect: run
//! stdout: 20100
module Conf;
import System;
public class H
  public n: int;
  public property P: int
    get => n;
    set => do
      var g := lambda(): int => value + 1;
      if value mod 3 = 0 then throw new InvalidOperationException("x"); end;
      n := g();
    end;
  end;
end H;
public class Program
  public static method Main()
    var i := 0;
    var tot := 0;
    using var h := new H();
    while i < 200 do
      try h.P := i; catch e: InvalidOperationException => tot := tot + 1; end;
      tot := tot + h.P;
      i := i + 1;
    end;
    Console.WriteLine(tot);
  end Main;
end Program;
