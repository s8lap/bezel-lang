//! case: l7_3_0002
//! doc: stdlib
//! section: 7.3
//! title: File.ReadAllText of a missing file raises FileNotFoundException, taken by a named arm
//! rule: "A whole-file read — `ReadAllText`, `ReadAllTextAsync`, `ReadAllLines`, `ReadAllBytes` — opens the file as §7.2's constructor does under `FileMode.Open` and `FileAccess.Read`, and an open that fails raises the same §7.5 types: `FileNotFoundException` when `path` names a file that does not exist"; "A read never returns a substitute value — an empty string or array — for a file it could not open."
//!   The path is relative, so its directory (the working directory) exists and the one condition is the missing file.
//! cites: 7.3, 7.2, 7.5
//! expect: run
//! output: not found
//!   done
//! issue: 7057
module Conf;

import System.IO;

public class Program
  public static method Main()
    try
      var text := File.ReadAllText("l7_3_0002-no-such-file.txt");
      Console.WriteLine($"read [{text}]");
    catch
      FileNotFoundException => Console.WriteLine("not found")
    end;
    Console.WriteLine("done");
  end Main;
end Program;
