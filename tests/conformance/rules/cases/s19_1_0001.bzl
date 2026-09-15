//! case: s19_1_0001
//! doc: spec
//! section: 19.1
//! title: a case with no scrutinee expression is a syntax error
//! rule: `case` always takes a scrutinee; a `case` written with no scrutinee
//!   expression is a syntax error — a ladder of conditions is an if/elif
//!   chain instead.
//! cites: 19.1
//! expect: reject
//! diagnostic: scrutinee
module Conf;

public class Program
  public static method Main()
    case of
      1 => Console.WriteLine("one"),
      _ => Console.WriteLine("other")
    end case;
  end Main;
end Program;
