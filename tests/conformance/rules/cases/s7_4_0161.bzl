//! case: s7_4_0161
//! doc: spec
//! section: 7.4
//! title: a const nullable function local tested against null is invocable in a lambda formed under the
//!   guard
//! rule: a fact no form can invalidate after the lambda's **formation** enters the body as it stands
//!   there — a `const` local's
//! cites: 27.5, 21.1
//! expect: run
//! output: 2
//! issue: 8099
module Conf;

public class Program
  public static method Pick(): ((int) -> int)?
    return lambda(x: int) => x + 1;
  end Pick;

  public static method Main()
    const h := Pick();
    if h <> null then
      var g := lambda(): int => h(1);
      Console.WriteLine(g());
    end if;
  end Main;
end Program;
