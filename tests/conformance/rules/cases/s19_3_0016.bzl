//! case: s19_3_0016
//! doc: spec
//! section: 19.3
//! title: a named slot list may name a subset of the record's members
//! rule: At least one slot is required, as for the positional form, and a named list
//!   may name any subset of the type's members: a member it omits is not read
//! cites: 19.3
//! expect: run
//! stdout: 4
//! issue: 2197
module Conf;


public record Rect(Width: int, Height: int);

public class Program
  public static method Main()
    var r := new Rect(3, 4);
    case r of
      Rect(Height = var h) => Console.WriteLine(h.ToString());,
      _                    => Console.WriteLine("other");
    end case;
  end Main;
end Program;
