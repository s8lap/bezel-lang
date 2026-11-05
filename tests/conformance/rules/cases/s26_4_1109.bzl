//! case: s26_4_1109
//! doc: spec
//! section: 26.4
//! title: an extension on Weak<T> with only where T: sendable is rejected (sendable is not class)
//! rule: by the parameters' constraints, which are the block's `where` clause and nothing else
//!   ... Composes with the other forms (`where T: sendable, class` admits sendable class
//!   types)
//! cites: 14.2
//! expect: reject
//! diagnostic: class
//! issue: 5998
module Conf;

extension Weak<T> where T: sendable
  public method P(): int
    return 1;
  end P;
end;

public class Program
  public static method Main()
  end Main;
end Program;
