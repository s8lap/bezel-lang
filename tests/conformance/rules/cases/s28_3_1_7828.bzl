//! case: s28_3_1_7828
//! doc: spec
//! section: 28.3.1
//! title: a Base-typed local holding a Derived, moved in a ?? right operand, is disposed on the skip path
//! rule: scope-end disposal per-path
//! cites: 7.7, 28.3.1
//! expect: run
//! output: 300
//!   0
module Conf;
import System;

public class Base
  public static Live: int := 0;
  public constructor()
    Base.Live := Base.Live + 1;
  end constructor;
  destructor
    Base.Live := Base.Live - 1;
  end destructor;
end Base;

public class Derived: Base
  public constructor()
    base();
  end constructor;
end Derived;

public class Sink
  public static method TakeB(owned b: Base): int
    using var h := move b;
    return 3;
  end TakeB;
end Sink;

public class Program
  public static method Run(k: int?): int
    var y: Base := new Derived();
    var t := k ?? Sink.TakeB(move y);
    return t;
  end Run;
  public static method Main(): int
    var s := 0;
    for var i in 0..<100 do
      s := s + Run(null);
      s := s + Run(0);
    end;
    Console.WriteLine(s);
    Console.WriteLine(Base.Live);
    return 0;
  end Main;
end Program;
