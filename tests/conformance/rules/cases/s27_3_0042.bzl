//! case: s27_3_0042
//! doc: spec
//! section: 27.3
//! title: a do-end block lambda with no value return is a procedure lambda at () -> ()
//! rule: A block-bodied lambda is a **procedure lambda** — its type is `(…) -> ()` — exactly when no path through its body executes a value-carrying `return e;` (v0.338 §27.3)
//! cites: 27.3
//! expect: run
//! output: after
module Conf;

public class Program
  public static method Main()
    var w: () -> () := lambda() => do end;
    w();
    Console.WriteLine("after");
  end Main;
end Program;
