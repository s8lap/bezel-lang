//! case: s30_7_0005
//! doc: spec
//! section: 30.7
//! title: await inside a source-written finally is rejected in an async iterator
//! rule: Inside an async iterator, `await` may not appear in a `finally`
//!   block — a compile error.
//! cites: 30.7
//! expect: reject
//! diagnostic: finally
module Conf;

public class Reader
  public constructor()
  end constructor;

  public async method NumbersAsync(): owned IAsyncEnumerable<int>
    try
      yield return 1;
    finally
      await Task.Delay(0);
    end try;
  end NumbersAsync;
end Reader;
