//! case: l6_10_0012
//! doc: stdlib
//! section: 6.10
//! title: an await using var declared at Object over a new TaskScope is rejected
//! rule: A `TaskScope` is minted only as the initializer of an `await using
//!   var` binding whose static type is `TaskScope` ... an owning position of
//!   a wider static type reached by a language spec §5.1 reference conversion
//!   (an `owned Object` return, an `owned IAsyncDisposable` parameter, an
//!   `await using var` declared at `Object` or at an interface).
//! cites: 6.10, 5.1, 28.12, 12.7
//! expect: reject
//! diagnostic: await using
//! issue: 5657
module Conf;

import System.Threading;

public class Program
  public static async method Run(): Task
    await using var scope: Object := new TaskScope();
    if scope is TaskScope then
      await scope.Join();
    end;
  end Run;
end Program;
