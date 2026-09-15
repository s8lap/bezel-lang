//! case: s19_1_0002
//! doc: spec
//! section: 19.1
//! title: a case with no of delimiter is a syntax error
//! rule: The keyword `of` terminates the scrutinee and introduces the arms.
//!   A missing `of` is a syntax error at the first arm token; the diagnostic
//!   names the delimiter.
//! cites: 19.1
//! expect: reject
//! diagnostic: of
module Conf;

public class Program
  public static method Main()
    var n := 3;
    case n
      1 => Console.WriteLine("one"),
      _ => Console.WriteLine("other")
    end case;
  end Main;
end Program;
