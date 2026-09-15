//! case: s28_3_0031
//! doc: spec
//! section: 28.3
//! title: a merge of an owned branch with a borrowed branch is rejected
//! rule: At a control-flow merge, owned meets borrowed is rejected: the program must
//!   equalize the branches before the merge; the compiler never equalizes for it.
//! cites: C.3
//! expect: reject
//! diagnostic: borrowed
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Registry
  private _w: Widget;

  public constructor(w: Widget)
    _w := w;
  end constructor;

  public property Current: Widget => _w;
end Registry;

public class Program
  public method Demo(useFresh: bool, reg: Registry)
    var x: Widget;
    if useFresh then
      x := new Widget();
    else
      x := reg.Current;
    end;
  end Demo;
end Program;
