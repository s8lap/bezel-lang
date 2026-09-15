//! case: s28_4_0005
//! doc: spec
//! section: 28.4
//! title: the destructor body runs exactly once, the first time Dispose is called
//! rule: A class may declare at most one destructor. Its body runs exactly once, the
//!   first time `Dispose()` is called.
//! expect: run
//! stdout: 1
module Conf;

public class Widget
  public static Count: int := 0;

  destructor
    Count := Count + 1;
  end destructor;
end Widget;

public class Program
  public static method Main()
    do
      using var w := new Widget();
      w.Dispose();
    end;
    Console.WriteLine($"{Widget.Count}");
  end Main;
end Program;
