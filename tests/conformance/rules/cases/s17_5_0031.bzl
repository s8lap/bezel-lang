//! case: s17_5_0031
//! doc: spec
//! section: 17.5
//! title: an enum value is admitted into an Array of its own enum and found by value
//! rule: "the left operand is admitted exactly when it is admissible as that call's argument under the ordinary conversion rules" ([§17.5]); an enum's default `Equals` is underlying integer equality ([§8.3]), so the element is found.
//! cites: 17.5, 8.3
//! expect: run
//! stdout: True
module Conf;

enum Col
  R, G;
end Col;

public class Program
  public static method Main()
    var a: Array<Col> := [Col.R];
    var found := Col.R in a;
    Console.WriteLine(found);
  end Main;
end Program;
