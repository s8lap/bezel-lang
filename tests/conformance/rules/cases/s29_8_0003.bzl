//! case: s29_8_0003
//! doc: spec
//! section: 29.8
//! title: the one enumerator disposes an owned parameter the body did not move out of
//! rule: The first `GetEnumerator()` transfers the frame's `owned` and
//!   move-only parameters to the enumerator it produces; that enumerator
//!   disposes any such parameter the body has not moved out of when it
//!   completes or is disposed.
//! cites: 29.8
//! expect: run
//! output: closed
//!   after
module Conf;

public class Lines
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("closed");
  end destructor;
end Lines;

public class Program
  public static method Echo(owned source: Lines): owned IEnumerable<int>
    yield return 1;
  end Echo;

  public static method Main()
    for var x in Echo(new Lines()) do
      _ := x;
    end;
    Console.WriteLine("after");
  end Main;
end Program;
