//! case: s26_5_0052
//! doc: spec
//! section: 26.5
//! title: an extension on Object applies to Task<T>, OwningTask<T> and class receivers
//! rule: for extension blocks on `x`'s type, base types, or implemented interfaces
//! cites: 26.2, 30.3, 8.1
//! expect: run
//! output: hi
//!   hi
//!   hi
module Conf;

extension Object
  public method Hello(): string => "hi";
end extension;

public class K
  public X: int := 1;
  public constructor() end constructor;
end K;

public class Program
  public static async method P(): OwningTask<K>
    return new K();
  end P;

  public static async method Main(): Task
    using var a := Task.FromResult(1);
    var o := P();
    Console.WriteLine(a.Hello());
    Console.WriteLine(o.Hello());
    using var k := await o;
    Console.WriteLine(k.Hello());
  end Main;
end Program;
