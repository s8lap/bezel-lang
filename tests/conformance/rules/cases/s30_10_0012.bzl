//! case: s30_10_0012
//! doc: spec
//! section: 30.10
//! title: Task.FromOwnedResult transfers the value to the awaiting context
//! rule: `Task.FromOwnedResult<T>(owned value: T): OwningTask<T> where T:
//!   class` — the caller transfers ownership to the task; awaiters consume the
//!   task under Pattern B, the await appearing in an owned-consuming context
//!   to which ownership transfers.
//! cites: 30.5
//! expect: run
//! output: gear
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Describe(): string
    return "gear";
  end Describe;
end Widget;

public class Program
  public static async method Main(): Task
    using var w := await Task.FromOwnedResult(new Widget());
    Console.WriteLine(w.Describe());
  end Main;
end Program;
