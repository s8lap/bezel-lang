//! case: s28_14_7516
//! doc: spec
//! section: 28.14
//! title: a capture-bearing closure passed to a scoped parameter of a plain var receiver is rejected
//! rule: A bounding object that is none of the admitted resolutions
//! cites: 28.14
//! expect: reject
//! diagnostic: bounding object
module Conf;
import System;
public class Counter
  public v: int := 0;
  public method Get(): int => v;
end Counter;
public class Holder
  private scoped _cb: (() -> int)?;
  public method Reg(scoped f: () -> int)
    _cb := f;
  end Reg;
  public method Fire(): int => _cb?() ?? -5;
end Holder;
public class Program
  public static method Main()
    using var c := new Counter();
    var h := new Holder();
    h.Reg(lambda() => c.Get());
    Console.WriteLine(h.Fire());
  end Main;
end Program;
