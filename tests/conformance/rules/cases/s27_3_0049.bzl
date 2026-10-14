//! case: s27_3_0049
//! doc: spec
//! section: 27.3
//! title: a block lambda that breaks out of a loop and falls off the end is rejected at () -> int
//! rule: A block-bodied lambda is a **procedure lambda** — its type is `(…) -> ()` — exactly when no path through its body executes a value-carrying `return e;`: its exits are bare `return;` and falling off the end. […] A target type, when present, is checked against this classification — a procedure-bodied lambda does not convert to a value-returning function type, nor the reverse. (v0.338 §27.3)
//! cites: 27.3
//! expect: reject
//! issue: 6283
//! diagnostic: not implicitly convertible
module Conf;

public class Program
  public static method Take(h: () -> int)
  end Take;

  public static method Main()
    var f := lambda() => do
      while true do break; end;
    end;
    Take(f);
  end Main;
end Program;
