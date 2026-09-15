//! case: s6_6_0006
//! doc: spec
//! section: 6.6
//! title: a compound assignment evaluates chain, getter, right operand and setter in that order
//! rule: The order is: the LHS chain's constituent expressions, then the final
//!   segment's getter (where applicable), then e, then the operation, then the
//!   store; when the final segment is accessor-mediated, its getter and setter
//!   are each invoked exactly once.
//! cites: 6.6, 17.6
//! expect: run
//! output: index
//!   get
//!   rhs
//!   set
module Conf;

public class Slots
  private owned _values: Array<int> := new Array<int>();

  public constructor()
    _values.Add(0);
  end constructor;

  public property [i: int]: int
    get => do
      Console.WriteLine("get");
      return _values[i];
    end;
    set => do
      Console.WriteLine("set");
      _values[i] := value;
    end;
  end;
end Slots;

public class Program
  public static method Index(): int
    Console.WriteLine("index");
    return 0;
  end Index;

  public static method Delta(): int
    Console.WriteLine("rhs");
    return 1;
  end Delta;

  public static method Main()
    using var s := new Slots();
    s[Program.Index()] += Program.Delta();
  end Main;
end Program;
