//! case: s21_5_0004
//! doc: spec
//! section: 21.5
//! title: a break inside a finally may not target a loop enclosing the finally
//! rule: Inside a `finally`, a `break` or `continue` — labeled or
//!   unlabeled — whose target loop lies outside the `finally` is a compile
//!   error.
//! cites: 21.5, 31.2
//! expect: reject
//! diagnostic: finally
module Conf;

public class Program
  public static method Main()
    loop: while true do
      try
        Console.WriteLine("try");
      finally
        break loop;
      end try;
    end loop;
  end Main;
end Program;
