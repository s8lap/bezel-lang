//! case: s19_5_0002
//! doc: spec
//! section: 19.5
//! title: a bare type pattern over a nullable reference scrutinee does not match null
//! rule: A bare type pattern `T` matches only non-null values.
//! cites: 19.5, 19.3
//! expect: run
//! output: null
//!   obj
module Conf;

public class Program
  public static method Classify(o: Object?): string => (case o of
    Object => "obj",
    _      => "null");

  public static method Main()
    Console.WriteLine(Classify(null));
    Console.WriteLine(Classify("a"));
  end Main;
end Program;
