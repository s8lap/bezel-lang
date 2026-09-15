//! case: s19_7_0004
//! doc: spec
//! section: 19.7
//! title: a statement-position case closes correctly with end case
//! rule: In statement position `case` is an `end`-terminated statement:
//!   `case … of … end case;`.
//! cites: 19.7
//! expect: run
//! stdout: Jan
module Conf;

public class Program
  public static method Main()
    var month := 1;
    case month of
      1 => Console.WriteLine("Jan"),
      _ => Console.WriteLine("Other")
    end case;
  end Main;
end Program;
