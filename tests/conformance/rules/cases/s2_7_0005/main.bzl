//! case: s2_7_0005
//! doc: spec
//! section: 2.7
//! title: the longest dotted prefix matching an imported module's full name
//!   resolves the chain
//! rule: Otherwise, match the longest dotted prefix of the chain against an
//!   imported module's full name. The remainder then resolves against that
//!   module's public declarations. `Data.Store` is the imported module's
//!   full (dotted) name; `Item` is the remainder.
//! cites: 2.7
//! expect: run
//! stdout: store-item
module Conf;
import Data.Store;

public class Program
  public static method Main()
    using var it := new Data.Store.Item();
    Console.WriteLine(it.Tag());
  end Main;
end Program;
