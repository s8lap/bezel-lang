//! case: s23_3_7626
//! doc: spec
//! section: 23.3
//! title: each call of a setter binds its own value parameter to the lambda formed in it
//! rule: The implicit setter parameter is `value`. It is a parameter of the setter
//! cites: 27.5
//! expect: run
//! stdout: 19900
module Conf;
import System;
public class H
  public n: int;
  public property P: int
    get => n;
    set => do var g := lambda(): int => 3; var q := g(); var k := lambda(): int => value; n := k(); end;
  end;
end H;
public class Program
  public static method Main()
    using var h := new H();
    var i := 0;
    var tot := 0;
    while i < 200 do h.P := i; tot := tot + h.P; i := i + 1; end;
    Console.WriteLine(tot);
  end Main;
end Program;
