//! case: s28_11_0003
//! doc: spec
//! section: 28.11
//! title: move on a borrowing collection slot is rejected — there is no ownership to transfer
//! rule: Array<T>, Dictionary<K, V>, and the other borrowing collections do
//!   not admit move coll[i] — their slots hold borrows, not owned values;
//!   there is no ownership to transfer.
//! cites: 28.11
//! expect: reject
//! diagnostic: move
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    using var arr: Array<Widget> := [new Widget()];
    var w := move arr[0];
  end Main;
end Program;
