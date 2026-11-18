//! case: s28_3_8_7607
//! doc: spec
//! section: 28.3.8
//! title: a capture-free lambda passed at a bare type parameter may be stored by the callee and invoked later
//! rule: At every set-erasing position a function value with a non-empty source set is rejected, and a capture-free one is admitted
//! cites: 28.3, 28.9
//! expect: run
//! stdout: 3
module Conf;
import System;
public class Keep<T>
  public F: T?;
  public constructor() end constructor;
  public method Put(a: T) F := a; end Put;
end Keep;
public class Program
  public static method Main(): int
    using var k := new Keep<() -> int>();
    for var i in 0 ..< 200 do
      var fq := lambda() => 3;
      k.Put(fq);
    end for;
    Console.WriteLine($"{k.F!()}");
    return 0;
  end Main;
end Program;
