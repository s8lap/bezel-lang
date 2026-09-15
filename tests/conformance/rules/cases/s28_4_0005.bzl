//! case: s28_4_0005
//! doc: spec
//! section: 28.4
//! title: the destructor body runs exactly once, the first time Dispose is called
//! rule: A class may declare at most one destructor. Its body runs exactly once, the
//!   first time `Dispose()` is called.
//! cites: 28.2
//! expect: run
//! output: disposed
module Conf;

public class Widget
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("disposed");
  end destructor;
end Widget;

public class Program
  public static method Main()
    do
      using var w := new Widget();
      w.Dispose();
    end;
  end Main;
end Program;
