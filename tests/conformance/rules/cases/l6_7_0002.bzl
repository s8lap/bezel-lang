//! case: l6_7_0002
//! doc: stdlib
//! section: 6.7
//! title: the owning WhenAll overload produces an owned array holding each task's result
//! rule: Task.WhenAll<T>(owned tasks: OwningArray<OwningTask<T>>):
//!   OwningTask<OwningArray<T>> where T: class — aggregates OwningTask<T>
//!   results into an owned array of owned elements.
//! cites: 6.7
//! expect: run
//! stdout: 1
module Conf;

public class Widget
  public readonly property Id: int;

  public constructor(id: int)
    Id := id;
  end constructor;
end Widget;

public class Program
  public static async method MakeAsync(id: int): OwningTask<Widget>
    return new Widget(id);
  end MakeAsync;

  public static async method Main(): Task
    var tasks := new OwningArray<OwningTask<Widget>>();
    tasks.Add(MakeAsync(7));
    using var results := await Task.WhenAll(move tasks);
    Console.WriteLine(results.Count);
  end Main;
end Program;
