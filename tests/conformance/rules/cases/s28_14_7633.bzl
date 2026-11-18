//! case: s28_14_7633
//! doc: spec
//! section: 28.14
//! title: a capture-free closure local passed to a generic sink then invoked and captured keeps working
//! rule: the callee receives the callback for the duration of the call only
//! cites: 28.9, 28.14
//! expect: run
//! stdout: 3400
module Conf;
import System;
public class Keep<T>
  public F: T?;
  public constructor() end constructor;
  public method Put(a: T): int F := a; return 7; end Put;
end Keep;
public class Program
  public static method Main(): int
    using var k := new Keep<() -> int>();
    var t := 0;
    for var i in 0 ..< 200 do
      var fq := lambda() => 3;
      t := t + k.Put(fq) + fq();
      var f3 := lambda() => k.Put(fq);
      t := t + f3();
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
