//! case: s28_11_0053
//! doc: spec
//! section: 28.11
//! title: move of a slot through a type alias of an owning inline array is admitted
//! rule: a slot of either owning inline-array form, `[N owned]T?` or `[N owned]T`
//! cites: 28.11, 15.1.1, 28.8.1, 27.8
//! expect: run
//! output: x
module Conf;

public class Res
  public Name: string;
  public constructor(n: string)
    this.Name := n;
  end constructor;
end Res;

type Pool := [2 owned]Res?;

public class Program
  public static method Main(): int
    var a: Pool := [new Res("x"), new Res("y")];
    using var t := move a[0];
    Console.WriteLine(t!.Name);
    return 0;
  end Main;
end Program;
