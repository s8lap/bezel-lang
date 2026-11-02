//! case: s22_12_0056
//! doc: spec
//! section: 22.12
//! title: a static readonly owning inline array is admitted, read and warned about
//! rule: An **owning-inline-array-typed** `static readonly` field takes the default
//!   warning
//! cites: 22.12, 15.1.1, 22.13
//! expect: run
//! warns: yes
//! output: x
module Conf;

public class Res
  public Name: string;
  public constructor(n: string)
    this.Name := n;
  end constructor;
end Res;

public class Registry
  public static readonly Pool: [2 owned]Res? := [new Res("x"), new Res("y")];
end Registry;

public class Program
  public static method Main(): int
    Console.WriteLine(Registry.Pool[0]!.Name);
    return 0;
  end Main;
end Program;
