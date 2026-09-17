//! case: s19_3_0007
//! doc: spec
//! section: 19.3
//! title: a named destructuring slot interrogates the member by name
//! rule: A named slot is `Identifier "=" Pattern`; the `=` is the
//!   interrogation reading of §8.4, not an assignment — "is `Width` 3?" —
//!   and a slot list may be written entirely by name.
//! cites: 19.3
//! expect: run
//! stdout: 4
module Conf;

public record Rect(Width: int, Height: int);

public class Program
  public static method Main()
    var r := new Rect(3, 4);
    case r of
      Rect(Width = 3, Height = var h) => Console.WriteLine(h.ToString());,
      _                               => Console.WriteLine("other");
    end case;
  end Main;
end Program;
