//! case: s27_3_0048
//! doc: spec
//! section: 27.3
//! title: a block lambda whose only exit is a bare return is a procedure lambda and is rejected at () -> int
//! rule: A block-bodied lambda is a **procedure lambda** — its type is `(…) -> ()` — exactly when no path through its body executes a value-carrying `return e;`: its exits are bare `return;` and falling off the end. […] A target type, when present, is checked against this classification — a procedure-bodied lambda does not convert to a value-returning function type, nor the reverse. (v0.338 §27.3)
//! cites: 27.3
//! expect: reject
//! diagnostic: not implicitly convertible
module Conf;

public class Program
  public static method Main()
    var f := lambda() => do return; end;
    var g: () -> int := f;
  end Main;
end Program;
