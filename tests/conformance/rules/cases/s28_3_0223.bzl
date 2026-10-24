//! case: s28_3_0223
//! doc: spec
//! section: 28.3
//! title: a fresh owned argument to a function value is discharged at the K8 boundary
//! rule: Unbound owned temporaries are disposed at their full expression's end,
//!   after their uses, in reverse production order — on the normal edge as on the
//!   exception edge, where the pass runs during unwinding (K8.1, §28.3, v0.343).
//! cites: 28.3, C.3
//! expect: run
//! output: DISPOSED
//!   after normal
//!   DISPOSED
//!   caught
//!   after throw
module Conf;

public class Item
  public constructor() end constructor;
  destructor Console.WriteLine("DISPOSED"); end destructor;
end Item;

public class Sink
  public method Accept(it: Item): int return 0; end Accept;
  public method Throw(it: Item): int throw new Exception("boom"); end Throw;
end Sink;

public class Program
  public static method Main(): int
    var s := new Sink();
    var f: (Item) -> int := s.Accept;
    _ := f(new Item());
    Console.WriteLine("after normal");
    var g: (Item) -> int := s.Throw;
    try
      _ := g(new Item());
    catch Exception =>
      Console.WriteLine("caught");
    end;
    Console.WriteLine("after throw");
    return 0;
  end Main;
end Program;
