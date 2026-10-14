//! case: s27_3_0050
//! doc: spec
//! section: 27.3
//! title: a block lambda with a value return and a throw is a value lambda at () -> int
//! rule: A block-bodied lambda is a **procedure lambda** — its type is `(…) -> ()` — exactly when no path through its body executes a value-carrying `return e;`: its exits are bare `return;` and falling off the end. (v0.338 §27.3)
//! cites: 27.3
//! expect: accept
module Conf;

public class Program
  public static method Probe()
    var f := lambda() => do
      if 1 > 0 then return 1; else throw new Exception("x"); end if;
    end;
    var g: () -> int := f;
  end Probe;
end Program;
