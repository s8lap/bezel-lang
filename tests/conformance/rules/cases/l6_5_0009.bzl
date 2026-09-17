//! case: l6_5_0009
//! doc: stdlib
//! section: 6.5
//! title: the producer-side += form registers a handler on the readonly source field
//! rule: The producer-side `_clicked += h` spelling remains available on the
//!   `readonly` field because the subscription forms neither read nor assign
//!   the operand; `lhs += h` rewrites to `lhs.Add(h)`.
//! cites: 6.5
//! expect: run
//! stdout: 7
module Conf;

public class Beeper
  private owned readonly _beep: EventSource<int> := new();

  public constructor()
    _beep += lambda(n) => Console.WriteLine(n);
  end constructor;

  public method Fire(n: int)
    _beep.Raise(n);
  end Fire;
end Beeper;

public class Program
  public static method Main()
    using var b := new Beeper();
    b.Fire(7);
  end Main;
end Program;
