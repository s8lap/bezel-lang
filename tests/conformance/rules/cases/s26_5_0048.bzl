//! case: s26_5_0048
//! doc: spec
//! section: 26.5
//! title: an extension on the bare Task applies to Task<T> and OwningTask<T> receivers
//! rule: for extension blocks on `x`'s type, base types, or implemented interfaces
//! cites: 30.3, 26.2
//! expect: run
//! output: 12312
module Conf;

public class K
  public constructor() end constructor;
end K;

extension Task
  public method Foo(): int => 1;
  public property Bar: int => 2;
end extension;

extension Task<T>
  public method Baz(): int => 3;
end extension;

public class Program
  public static async method P(): OwningTask<K>
    return new K();
  end P;

  public static method Main(): int
    using var a := Task.FromResult(1);
    using var c := P();
    Console.WriteLine(a.Foo().ToString() + a.Bar.ToString() + a.Baz().ToString() + c.Foo().ToString() + c.Bar.ToString());
    return 0;
  end Main;
end Program;
