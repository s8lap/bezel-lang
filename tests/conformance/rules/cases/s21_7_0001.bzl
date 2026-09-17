//! case: s21_7_0001
//! doc: spec
//! section: 21.7
//! title: a method with a return type must return on every exit path
//! rule: A method or accessor with a return type must `return expression;`
//!   on every exit path.
//! cites: 21.7
//! expect: reject
//! diagnostic: return
module Conf;

public class Program
  public static method Sign(n: int): string
    if n > 0 then
      return "positive";
    end if;
  end Sign;

  public static method Main()
    Console.WriteLine(Sign(1));
  end Main;
end Program;
