//! case: s2_3_0050
//! doc: spec
//! section: 2.3
//! title: A library method taking and returning the System DayOfWeek is callable from an importer
//! rule: The auto-imported System DayOfWeek (§2.3, v0.350; stdlib §2.5) is one type across modules: Lib3.L3.Id(DayOfWeek) called with Wednesday returns Wednesday, byte value 3.
//! cites: 2.3, 2.5, 2.8
//! expect: run
//! stdout: 3
module App;

import Lib3;

public class Program
  public static method Main()
    Console.WriteLine(int(byte(L3.Id(DayOfWeek.Wednesday))));
  end Main;
end Program;
