//! case: s30_5_0004
//! doc: spec
//! section: 30.5
//! title: move is rejected on a Pattern B await operand
//! rule: The position is **marker-free** — the transfer marker is not
//!   admitted where the target already announces the consume; Pattern B
//!   await therefore deliberately joins no consume-site freeze list and
//!   takes no operand marker.
//! cites: 30.5, 28.3
//! expect: reject
//! diagnostic: marker-free
//! issue: 4670
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public async method HandleAsync(): Task
    var t := Task.FromOwnedResult<Widget>(new Widget());
    using var r := await move t;
  end HandleAsync;
end Program;
