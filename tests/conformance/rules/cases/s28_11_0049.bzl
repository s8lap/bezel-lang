//! case: s28_11_0049
//! doc: spec
//! section: 28.11
//! title: move of a slot of an owning inline-array local takes it and leaves null
//! rule: a slot of either owning inline-array form, `[N owned]T?` or `[N owned]T`
//! cites: 28.11, 15.1.1, 28.8.1
//! expect: run
//! output: y
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
    using var t := move a[1];
    Console.WriteLine(t!.Name);
    return 0;
  end Main;
end Program;
