//! case: s28_11_0052
//! doc: spec
//! section: 28.11
//! title: move of owning inline-array slots in a loop takes each in order
//! rule: a slot of either owning inline-array form, `[N owned]T?` or `[N owned]T`
//! cites: 28.11, 15.1.1, 28.8.1
//! expect: run
//! output: x
//!   y
module Conf;

public class Res
  public Name: string;
  public constructor(n: string)
    this.Name := n;
  end constructor;
end Res;

public class Program
  public static method Main(): int
    var a: [2 owned]Res? := [new Res("x"), new Res("y")];
    for var i in 0..<2 do
      using var x := move a[i];
      Console.WriteLine(x!.Name);
    end;
    return 0;
  end Main;
end Program;
