//! case: s22_6_0009
//! doc: spec
//! section: 22.6
//! title: example 6 -- applicability filtering removes an inapplicable candidate
//! rule: "byte→sbyte is NOT implicit ... ⇒ Process(sbyte) is not
//!   applicable. byte→ushort IS implicit ... ⇒ Process(ushort) is the
//!   only applicable candidate."
//! cites: 22.6, 5.1
//! expect: run
//! stdout: ushort
module Conf;

public class Handler
  public constructor()
  end constructor;

  public method Process(n: sbyte): string => "sbyte";
  public method Process(n: ushort): string => "ushort";
end Handler;

public class Program
  public static method Main()
    var h := new Handler();
    var val: byte := 50;
    Console.WriteLine(h.Process(val));
  end Main;
end Program;
