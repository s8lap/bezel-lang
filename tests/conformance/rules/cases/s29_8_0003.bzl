//! case: s29_8_0003
//! doc: spec
//! section: 29.8
//! title: iterator disposal disposes an owned parameter the body did not move out of
//! rule: When the iterator is disposed, the state machine disposes any
//!   `owned` parameter that has not been moved out of by the body.
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
