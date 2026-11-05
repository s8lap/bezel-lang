//! case: s26_4_1105
//! doc: spec
//! section: 26.4
//! title: a generic extension on Channel<T> with no where clause is rejected
//! rule: `extension OwningArray<T>` is a compile error naming the constraint and `T`, `extension
//!   OwningArray<T> where T: class` is admitted ... by the parameters' constraints, which are
//!   the block's `where` clause and nothing else
//! cites: 14.2
//! expect: reject
//! diagnostic: sendable
//! issue: 5998
module Conf;
import System.Threading;

extension Channel<T>
  public method P(): int
    return 1;
  end P;
end;

public class Program
  public static method Main()
  end Main;
end Program;
