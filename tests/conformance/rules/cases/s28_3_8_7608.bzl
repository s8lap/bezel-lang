//! case: s28_3_8_7608
//! doc: spec
//! section: 28.3.8
//! title: a capture-free closure local passed repeatedly at a bare type parameter stays callable
//! rule: At every set-erasing position a function value with a non-empty source set is rejected, and a capture-free one is admitted
//! cites: 28.3, 28.9
//! expect: run
//! stdout: 600
module Conf;
import System;
public class Keep<T>
  public constructor() end constructor;
  public method Put(a: T) end Put;
  public method Put2(a: T, b: T) end Put2;
end Keep;
public class Program
  public static method Main(): int
    using var k := new Keep<() -> int>();
    var t := 0;
    for var i in 0 ..< 200 do
      var fq := lambda() => 3;
      k.Put(fq);
      k.Put(fq);
      k.Put2(fq, fq);
      t := t + fq();
    end for;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;
