//! case: s19_7_0001
//! doc: spec
//! section: 19.7
//! title: a statement-position case is closed with end case not end switch
//! rule: In statement position `case` is an end-terminated statement
//!   (`case … of … end case?;`); `end switch;` is a near-miss the closer
//!   rejects.
//! cites: 19.7, 21
//! expect: reject
//! diagnostic: case
module Conf;

public class Program
  public static method Main()
    var month := 1;
    case month of
      1 => Console.WriteLine("Jan"),
      _ => Console.WriteLine("Other")
    end switch;
  end Main;
end Program;
