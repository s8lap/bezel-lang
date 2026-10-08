//! case: s30_1_0021
//! doc: spec
//! section: 30.1
//! title: a borrow-bearing local with a frame-resident source may cross a suspension point
//! rule: A local of borrow-bearing record or borrow-bearing inline-array
//!   type ... is admitted across the
//!   suspension iff every borrow it carries has a frame-resident source: an
//!   owned local, a `using var`/`await using var`, or an `owned` parameter
//!   of the same async body.
//! cites: 30.1, 9.7
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

record Wrap(W: Widget);

public class Program
  public async method HandleAsync(owned w: Widget): Task
    using var taken := move w;
    var wrap := new Wrap(taken);
    await Task.Delay(0);
    Use(wrap);
  end HandleAsync;

  private method Use(w: Wrap)
  end Use;
end Program;
