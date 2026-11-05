//! case: s26_4_1106
//! doc: spec
//! section: 26.4
//! title: a generic extension on Channel<T> with only where T: class is rejected
//! rule: by the parameters' constraints, which are the block's `where` clause and nothing else
//!   ... Composes with the other forms (`where T: sendable, class` admits sendable class
//!   types)
//! cites: 14.2
//! expect: reject
//! diagnostic: sendable
//! issue: 5998
module Conf;
import System.Threading;

extension Channel<T> where T: class
  public method P(): int
    return 1;
  end P;
end;

public class Program
  public static method Main()
  end Main;
end Program;
