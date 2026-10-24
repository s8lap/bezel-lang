//! case: s28_3_0225
//! doc: spec
//! section: 28.3
//! title: a fresh owned argument to a function value with an interface formal is discharged at the K8 boundary
//! rule: Unbound owned temporaries are disposed at their full expression's end,
//!   after their uses, in reverse production order (K8.1, §28.3, v0.343).
//! cites: 28.3, C.3
//! expect: run
//! output: item disposed
//!   after call
//! issue: 7125
module Conf;

public class Item
  public constructor() end constructor;
  destructor Console.WriteLine("item disposed"); end destructor;
end Item;

public class Sink
  public method Accept(it: IDisposable): int return 0; end Accept;
end Sink;

public class Program
  public static method Main(): int
    var s := new Sink();
    var f: (IDisposable) -> int := s.Accept;
    _ := f(new Item());
    Console.WriteLine("after call");
    return 0;
  end Main;
end Program;
