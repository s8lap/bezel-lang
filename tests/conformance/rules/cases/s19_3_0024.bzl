//! case: s19_3_0024
//! doc: spec
//! section: 19.3
//! title: a naked name in a positional slot under a named slot is a binding
//! rule: The positional naked-binding shorthand is scoped to positional slots: Inner(W) binds W, so every value selects the arm. A binding in a named slot is spelled var w, but a naked name inside a positional sub-pattern under a named slot still binds.
//! cites: 19.3
//! expect: run
//! stdout: bind
//! issue: 6037
module Conf;

public record Inner(X: int);

public record Outer(I: Inner);

public class Program
  public static method Main()
    const W: int := 3;
    var b := new Outer(new Inner(5));
    case b of Outer(I = Inner(W)) => Console.WriteLine("bind");,
      _ => Console.WriteLine("n");
    end case;
  end Main;
end Program;
