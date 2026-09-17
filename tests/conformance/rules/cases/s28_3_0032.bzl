//! case: s28_3_0032
//! doc: spec
//! section: 28.3
//! title: equalizing both branches to owned admits the merge
//! rule: Producing an owned value on both branches equalizes the merge to owned.
//! expect: accept
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

  public method CloneCurrent(): owned Widget => new Widget();
end Registry;

public class Program
  public method Demo(useFresh: bool, reg: Registry)
    var x: Widget;
    if useFresh then
      x := new Widget();
    else
      x := reg.CloneCurrent();
    end;
    x.Dispose();
  end Demo;
end Program;
