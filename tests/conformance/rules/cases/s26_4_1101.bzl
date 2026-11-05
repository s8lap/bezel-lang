//! case: s26_4_1101
//! doc: spec
//! section: 26.4
//! title: a generic extension on Channel<T> with where T: sendable is admitted
//! rule: `extension OwningArray<T>` is a compile error naming the constraint and `T`, `extension
//!   OwningArray<T> where T: class` is admitted ... by the parameters' constraints, which are
//!   the block's `where` clause and nothing else
//! cites: 14.2
//! expect: accept
//! issue: 5998
module Conf;
import System.Threading;

extension Channel<T> where T: sendable
  public method P(): int
    return 1;
  end P;
end;
