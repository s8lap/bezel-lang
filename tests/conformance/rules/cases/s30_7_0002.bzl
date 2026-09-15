//! case: s30_7_0002
//! doc: spec
//! section: 30.7
//! title: await for over IOwningAsyncEnumerable<T> yields an owned iteration variable
//! rule: Each iteration variable is owned; the §28.3 discharge rules apply
//!   on every exit edge of the loop body — the same rule as for synchronous
//!   `for` over `IOwningEnumerable<T>`.
//! cites: 30.7, 29.2
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Factory
  public constructor()
  end constructor;

  public async method MakeAllAsync(): owned IOwningAsyncEnumerable<Widget>
    await Task.Delay(0);
    yield return new Widget();
  end MakeAllAsync;
end Factory;

public class Program
  public async method ProcessAllAsync(): Task
    await for var w in new Factory().MakeAllAsync() do
      using var taken := move w;
    end;
  end ProcessAllAsync;
end Program;
