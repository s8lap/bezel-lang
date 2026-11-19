//! case: s7_5_7731
//! doc: spec
//! section: 7.5
//! title: a null-conditional getter read evaluates the getter once per read
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access
//! cites: 14.1, 23.3
//! expect: run
//! stdout: 3 6
module Conf;
import System;
public class Counter
  public n: int;
  public constructor() n := 0; end constructor;
end Counter;
public class Box<T>
  public v: T;
  public c: Counter;
  public constructor(a: T, k: Counter) v := a; c := k; end constructor;
  public property P: T
    get => do c.n := c.n + 1; return v; end;
  end P;
end Box;
public static class Program
  public static method Main(args: Array<string>): int
    using var k := new Counter();
    using var bx := new Box<string>("ab", k);
    var bn: Box<string>? := bx;
    var t := 0;
    for var i in 0..<3 do
      var y := bn?.P;
      if y <> null then t := t + y!.Length; end;
    end;
    Console.WriteLine($"{k.n} {t}");
    return 0;
  end Main;
end Program;
