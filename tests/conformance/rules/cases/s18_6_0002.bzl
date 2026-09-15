//! case: s18_6_0002
//! doc: spec
//! section: 18.6
//! title: a doubled quote inside a verbatim string produces a single embedded quote
//! rule: "Inside a verbatim string, `\"\"` produces a single `\"`."
//! cites: 18.6
//! expect: run
//! stdout: She said "hi"
module Conf;

public class Program
  public static method Main()
    var s := @"She said ""hi""";
    Console.WriteLine(s);
  end Main;
end Program;
