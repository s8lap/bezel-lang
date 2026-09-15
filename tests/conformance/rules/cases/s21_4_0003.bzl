//! case: s21_4_0003
//! doc: spec
//! section: 21.4
//! title: a type ascription on the ignore for-loop variable is rejected
//! rule: A type ascription on the ignore is rejected (`for var _: T in …`
//!   — a bare ignore takes its type from context, never an annotation).
//! cites: 21.4, 17.13
//! expect: reject
//! diagnostic: ignore
module Conf;

public class Program
  public static method Main()
    var nums: Array<int> := [1, 2, 3];
    for var _: int in nums do
      Console.WriteLine("tick");
    end;
  end Main;
end Program;
