//! case: s22_16_0025
//! doc: spec
//! section: 22.16
//! title: requires clauses evaluate in declaration order and stop at the first false
//! rule: In declaration order, stopping at the first clause producing
//!   `false` — clause granularity is semantic: the clause is the unit of
//!   ... evaluation order (stop at first failure).
//! cites: 22.16
//! expect: run
//! stdout: caught
module Conf;

public class Ops
  public static method Div(x: int): int
    requires(x > 0)
    requires(10 / x > 1)

    return 10 / x;
  end Div;
end Ops;

public class Program
  public static method Main()
    try
      Ops.Div(0);
    catch
      var ex: ContractViolationException => Console.WriteLine("caught"),
    end;
  end Main;
end Program;
