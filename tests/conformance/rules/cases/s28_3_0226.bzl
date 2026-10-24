//! case: s28_3_0226
//! doc: spec
//! section: 28.3
//! title: a fresh owned argument to a function value is discharged after the callee's use
//! rule: Unbound owned temporaries are disposed at their full expression's end,
//!   after their uses, in reverse production order (K8.1, §28.3, v0.343). The
//!   temporary is a plain (borrowing) argument of the function value, so the
//!   callee's use of it precedes the disposal.
//! cites: 28.3, C.3
//! expect: run
//! output: got 4
//!   item disposed
//!   after call
module Conf;

public class Item
  public v: int := 4;

  public constructor() end constructor;

  destructor Console.WriteLine("item disposed"); end destructor;

  public method Get(): int => this.v;
end Item;

public class Sink
  public method Accept(it: Item?): int
    if it <> null then
      Console.WriteLine("got " + it.Get().ToString());
    end if;
    return 0;
  end Accept;
end Sink;

public class Program
  public static method Main(): int
    var s := new Sink();
    var f: (Item?) -> int := s.Accept;
    _ := f(new Item());
    Console.WriteLine("after call");
    return 0;
  end Main;
end Program;
