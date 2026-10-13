//! case: s28_3_0202
//! doc: spec
//! section: 28.3
//! title: a caller keeps a static witness's result as a borrow within its sources' scope
//! rule: A `static` method or `operator` declaration ... whose declared return
//!   type is a class or interface type is a **compile error** unless the
//!   declared type itself announces the classification ... — a static method
//!   implementing a static interface member so declared (§11.8) takes that
//!   row too: its witness signature is spelled at the conforming type and
//!   classified at the interface member ..., its return paths are checked
//!   under row 5's callee-side admission against the positionally
//!   corresponding source parameters
//! cites: 28.3, 11.8
//! expect: run
//! stdout: 1
//! issue: 5818
module Conf;

public interface IPick<T>
  static method Pick(a: T, b: T): T;
end IPick;

public class Item: IPick<Item>
  public constructor()
  end constructor;

  public static method Pick(a: Item, b: Item): Item => a;

  public method Size(): int => 1;
end Item;

public class Program
  public static method Main()
    using var a := new Item();
    using var b := new Item();
    var r := Item.Pick(a, b);
    Console.WriteLine(r.Size());
  end Main;
end Program;
