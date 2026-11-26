//! case: s3_4_3601
//! doc: spec
//! section: 3.4
//! title: a local named String is a compile error at the declaration
//! rule: **No binding takes one of these names either**: a local of any declaration form …, a parameter
//!   of a method, constructor or lambda, a pattern binding, a `catch` arm's binding and a type parameter
//!   named one of them is a compile error at the declaration
//! cites: 3.4, 2.3, 2.7
//! expect: reject
//! diagnostic: String
//! issue: 8115
module Conf;

public class Program
  public static method Main()
    var String := 5;
    Console.WriteLine(String + 1);
  end Main;
end Program;
