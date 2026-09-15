//! case: s17_11_0002
//! doc: spec
//! section: 17.11
//! title: default(bool) is false
//! rule: "`bool`: `false`."
//! cites: 17.11
//! expect: run
//! stdout: False
module Conf;

public class Program
  public static method Main()
    var b := default(bool);
    Console.WriteLine(b);
  end Main;
end Program;
