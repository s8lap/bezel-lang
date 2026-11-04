//! case: s2_2_0044
//! doc: spec
//! section: 2.2
//! title: an alias-qualified name in type position that is no public type of the aliased module is an error
//! rule: thereafter `Tx.Ledger` names that module's public `Ledger`. The alias is an additional
//!   *qualifier*, not a rename
//! cites: 2.2, 2.7
//! expect: reject
//! diagnostic: has no public type
module Conf;
import System.IO as sio;

public class Program
  private static method Use(x: sio.NotAType)
  end Use;

  public static method Main()
    Console.WriteLine("after");
  end Main;
end Program;
