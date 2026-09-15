//! case: s18_8_0002
//! doc: spec
//! section: 18.8
//! title: concatenation is the remedy to write a two-digit hex escape followed by literal characters
//! rule: "To produce the two-digit escape `\\x41` followed by the literal
//!   characters `BC`, use concatenation (`\"\\x41\" + \"BC\"`) or the `\\u` form."
//! cites: 18.8
//! expect: run
//! stdout: ABC
module Conf;

public class Program
  public static method Main()
    var s := "\x41" + "BC";
    Console.WriteLine(s);
  end Main;
end Program;
