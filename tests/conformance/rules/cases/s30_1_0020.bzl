//! case: s30_1_0020
//! doc: spec
//! section: 30.1
//! title: a borrow-bearing local with a caller-side source may not cross a suspension point
//! rule: A local of borrow-bearing record or borrow-bearing inline-array
//!   type ... is admitted across the
//!   suspension iff every borrow it carries has a frame-resident source ...
//!   A borrow whose source is caller-side — a plain parameter, `this`, a
//!   field reached through `this` — does not qualify.
//! cites: 30.1, 9.7
//! expect: reject
//! diagnostic: suspension
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

record Wrap(W: Widget);

public class Program
  public async method HandleAsync(w: Widget): Task
    var wrap := new Wrap(w);
    await Task.Delay(0);
    Use(wrap);
  end HandleAsync;

  private method Use(w: Wrap)
  end Use;
end Program;
