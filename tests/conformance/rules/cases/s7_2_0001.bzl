//! case: s7_2_0001
//! doc: spec
//! section: 7.2
//! title: a nullable value type either holds a value or null
//! rule: A value type wrapped in `?` is an optional: it either holds a value
//!   or `null`.
//! expect: run
//! stdout: no score
module Conf;

public class Program
  public static method Main()
    var score: int? := null;
    if score = null then
      Console.WriteLine("no score");
    else
      Console.WriteLine("has score");
    end;
  end Main;
end Program;
