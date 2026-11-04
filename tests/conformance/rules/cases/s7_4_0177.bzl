//! case: s7_4_0177
//! doc: spec
//! section: 7.4
//! title: a static readonly function field rooted at a type name enters a lambda formed under the guard
//! rule: a `readonly`-field path's rooted at a `const` local, at `this` or at a type name
//! cites: 27.5
//! expect: run
//! output: 11
//! issue: 8099
module Conf;

public class H
  public static readonly S: ((int) -> int)? := lambda(x: int) => x + 10;
end H;

public class Program
  public static method Main()
    if H.S is (int) -> int then
      var g := lambda(): int => H.S(1);
      Console.WriteLine(g());
    end if;
  end Main;
end Program;
