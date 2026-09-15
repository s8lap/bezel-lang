//! case: s26_5_0011
//! doc: spec
//! section: 26.5
//! title: an extension's own module must be imported even when its type is only used
//! rule: Extension members are the one exception to use-without-import: they
//!   are candidates only from the current module and imported modules
//!   (§2.8.3 rule 2). `Widget` reaches `App` transitively through `Factory`
//!   with no import needed to *use* it, but `Doubled` lives in `Lib`, which
//!   `App` never imports, so it is not a candidate.
//! cites: 26.5, 2.8.3
//! expect: reject
//! diagnostic: Doubled
module App;
import Factory;

public class Program
  public static method Main()
    using var w := WidgetFactory.Make(6);
    Console.WriteLine(w.Doubled());
  end Main;
end Program;
