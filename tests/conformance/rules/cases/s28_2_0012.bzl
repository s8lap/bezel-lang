//! case: s28_2_0012
//! doc: spec
//! section: 28.2
//! title: the same program under the explicitly selected unchecked relaxation is accepted only
//! rule: Unchecked (release) mode — use of a disposed instance is undefined
//!   behavior; a build selects it explicitly. A program that would raise
//!   `DisposedAccessException` in checked mode has undefined behavior in
//!   unchecked mode: unchecked mode is the release-build relaxation of the
//!   default checked semantics, not a different language.
//! cites: 28.2, 28.10
//! expect: accept
//! flags: --unchecked-disposal
module Conf;

public class Widget
  public property Label: string;

  public constructor()
    Label := "live";
  end constructor;

  destructor
  end destructor;
end Widget;

public class Program
  public method Demo()
    var w := new Widget();
    using var view := w;
    w.Dispose();
    try
      Console.WriteLine(view.Label);
    catch
      DisposedAccessException => Console.WriteLine("faulted"),
    end try;
  end Demo;
end Program;
