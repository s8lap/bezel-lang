//! case: s23_3_7627
//! doc: spec
//! section: 23.3
//! title: a closure formed in one setter call keeps that call's value after a later call
//! rule: The implicit setter parameter is `value`. It is a parameter of the setter
//! cites: 27.5
//! expect: run
//! stdout: 19701
module Conf;
import System;
public class H
  public f: () -> int := lambda(): int => 0;
  public property P: int
    get => 0;
    set => do var g := lambda(): int => 3; var q := g(); f := lambda(): int => value; end;
  end;
end H;
public class Program
  public static method Main()
    using var h := new H();
    var tot := 0;
    var i := 0;
    while i < 200 do var prev := h.f; h.P := i; tot := tot + prev(); i := i + 1; end;
    Console.WriteLine(tot);
  end Main;
end Program;
