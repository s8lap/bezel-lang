//! case: s28_3_8_8601
//! doc: spec
//! section: 28.3.8
//! title: a capture-free function value swapped through a generic ref pair is admitted at an Object parameter
//! rule: At every set-erasing position a function value with a non-empty source set is rejected, and a capture-free one is admitted
//! cites: 28.3.8
//! expect: run
//! stdout: 8
//! notes: Harvested from the #9075 review (probe a14, base-green boundary of the swap fix). f and g
//!   hold capture-free lambdas, so after SwapG the local f holds the lambda that returns 8 and
//!   its source set is empty; Keep(f) at the Object parameter is admitted and Call() runs it.
module Conf;
import System;
public class Reg
  private _o: Object? := null;
  public constructor() end constructor;
  public method Keep(o: Object)
    _o := o;
  end Keep;
  public method Call(): int
    if _o is () -> int then
      return (_o as () -> int)!();
    end if;
    return -1;
  end Call;
end Reg;
public class Program
  public static method SwapG<T>(ref a: T, ref b: T)
    var t: T := a;
    a := b;
    b := t;
  end SwapG;
  public static method Main(): int
    using var reg := new Reg();
    var f: () -> int := lambda(): int => 9;
    var g: () -> int := lambda(): int => 8;
    Program.SwapG(ref f, ref g);
    reg.Keep(f);
    Console.WriteLine(reg.Call());
    return 0;
  end Main;
end Program;
