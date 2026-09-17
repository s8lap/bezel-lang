//! case: s29_7_0002
//! doc: spec
//! section: 29.7
//! title: partial-consumption disposal runs pending cleanups in reverse order of establishment
//! rule: Runs the pending cleanup actions active at the point of suspension
//!   — `using var` disposals ... and `finally` clauses — in reverse order of
//!   establishment, exactly as if the body had exited at the suspension
//!   point.
//! cites: 29.7
//! expect: run
//! output: b
//!   a
module Conf;

public class Tag
  private _name: string;

  public constructor(name: string)
    _name := name;
  end constructor;

  destructor
    Console.WriteLine(_name);
  end destructor;
end Tag;

public class Program
  public static method Gen(): IEnumerable<int>
    using var a := new Tag("a");
    using var b := new Tag("b");
    yield return 1;
  end Gen;

  public static method Main()
    for var x in Gen() do
      break;
    end;
  end Main;
end Program;
