//! case: s2_3_9906
//! doc: spec
//! section: 2.3
//! title: a qualified imported FrozenArray<T> keeps its own writable indexer
//! rule: a same-named public exported by an explicitly imported module does not shadow it but makes the
//!   bare name **ambiguous** in that file
//! cites: 2.3, 2.2, 2.7
//! expect: run
//! stdout: 3
//! issue: 9251
//! notes: Harvested from the #9251 review (imp/main). Importing Lib makes the bare name FrozenArray
//!   ambiguous (2.3), so the program reaches the library's class by qualification; that class
//!   declares a settable indexer, so the write is admitted: 7 becomes 3.
module App;
import Lib;

public class Program
  public static method Main()
    var s := new Array<int>();
    s.Add(7);
    var f := new Lib.FrozenArray<int>(s);
    f[0] := 3;
    Console.WriteLine(f[0]);
  end Main;
end Program;
