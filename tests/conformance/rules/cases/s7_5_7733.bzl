//! case: s7_5_7733
//! doc: spec
//! section: 7.5
//! title: a null-conditional call of a function-typed member skips its arguments on a null receiver
//! rule: the argument and index expressions of the skipped suffixes are not evaluated either
//! cites: 27.1
//! expect: run
//! stdout: 17 6 1001 calls=0
module Conf;
import System;
public class Cnt
  public n: int := 0;
end Cnt;
public static class U
  public static method Bump(c: Cnt): int
    c.n := c.n + 1;
    return 1;
  end Bump;
end U;
public class H
  private n: int;
  public constructor(k: int) n := k; end constructor;
  public property P: (int) -> int
    get => do
      var k := n;
      return lambda(x: int): int => x + k;
    end;
  end P;
  public F: (int) -> int := lambda(x: int) => x * 2;
  public method Meth(x: int): int => x + 1000;
end H;
public static class Program
  public static method Main(args: Array<string>): int
    using var cnt := new Cnt();
    using var h := new H(7);
    var hn: H? := h;
    var hz: H? := null;
    var a := hn?.P(10);
    var b := hn?.F(3);
    var c := hn?.Meth(1);
    var d := hz?.P(U.Bump(cnt));
    var e := hz?.F(U.Bump(cnt));
    if d <> null or e <> null then Console.WriteLine("bad"); end;
    Console.WriteLine($"{a!} {b!} {c!} calls={cnt.n}");
    return 0;
  end Main;
end Program;
