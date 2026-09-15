//! case: s28_2_0001
//! doc: spec
//! section: 28.2
//! title: Dispose is idempotent: a second explicit call is a no-op
//! rule: `Dispose()` on any class instance is idempotent: the first call performs the
//!   disposal; subsequent calls are no-ops.
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
    var w := new Widget();
    w.Dispose();
    w.Dispose();
    Console.WriteLine($"{Widget.Count}");
  end Main;
end Program;
