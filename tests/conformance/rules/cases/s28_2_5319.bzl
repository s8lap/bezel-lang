//! case: s28_2_5319
//! doc: spec
//! section: 28.2
//! title: a second Dispose() of a using var StringBuilder is a runtime no-op, not an error
//! rule: A further `Dispose()` call remains a no-op, not an error, in both modes.
//! cites: 28.2, 28.6
//! expect: run
//! warns: yes
//! output: 200
//! flags: --checked-disposal
module Conf;

public class Program
  public static method Main()
    var total := 0;
    for var i in 0..<200 do
      using var c := new StringBuilder();
      total := total + 1;
      c.Dispose();
      c.Dispose();
    end;
    Console.WriteLine(total);
  end Main;
end Program;
