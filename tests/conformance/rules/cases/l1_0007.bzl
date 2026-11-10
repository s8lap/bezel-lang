//! case: l1_0007
//! doc: stdlib
//! section: 1
//! title: a missing key at the indexer passes an IFailure arm
//! rule: `Parse` raises a failure, because the text is the outside's, where the
//!   indexer raises a defect, because the dictionary is the program's.
//! cites: 1, 4.8
//! expect: run
//! stdout: defect passed
//! issue: 8414
module Conf;

public class Program
  public static method Main()
    using var d := new Dictionary<string, int>();
    try
      try
        _ := d["missing"];
      catch
        IFailure => Console.WriteLine("taken as a failure")
      end;
    catch
      KeyNotFoundException => Console.WriteLine("defect passed")
    end;
  end Main;
end Program;
