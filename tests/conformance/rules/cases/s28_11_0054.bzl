//! case: s28_11_0054
//! doc: spec
//! section: 28.11
//! title: move of an owning inline-array slot after an await in an async method is admitted
//! rule: a slot of either owning inline-array form, `[N owned]T?` or `[N owned]T`
//! cites: 28.11, 15.1.1, 28.8.1, 30.1
//! expect: accept
module Conf;

public class Res
  public Name: string;
  public constructor(n: string)
    this.Name := n;
  end constructor;
end Res;

public class Program
  public static async method Go(): Task<int>
    var a: [2 owned]Res? := [new Res("x"), new Res("y")];
    await Task.Delay(1);
    using var t := move a[1];
    Console.WriteLine(t!.Name);
    return 1;
  end Go;
end Program;
