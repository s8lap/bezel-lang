//! case: s30_5_0014
//! doc: spec
//! section: 30.5
//! title: Task.FromOwnedResult produces a Pattern B task for a class-typed T
//! rule: For class T, use `Task.FromOwnedResult<T>(owned value: T):
//!   OwningTask<T>` instead. The caller transfers ownership of `value` to
//!   the task; the produced task is Pattern B.
//! cites: 30.5, 30.10
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static async method LoadAsync(): OwningTask<Widget>
    using var t := Task.FromOwnedResult<Widget>(new Widget());
    return await t;
  end LoadAsync;
end Program;
