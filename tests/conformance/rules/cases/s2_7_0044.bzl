//! case: s2_7_0044
//! doc: spec
//! section: 2.7
//! title: a local named like an import alias shadows the alias in a dotted name
//! rule: local scope, then the enclosing type's accessible members
//! cites: 2.2, 2.7
//! expect: run
//! stdout: 3
module Conf;
import System.IO as sio;

public class W
  public File: int;
end W;

public class Program
  public static method Main()
    using var sio := new W();
    sio.File := 3;
    Console.WriteLine(sio.File);
  end Main;
end Program;
