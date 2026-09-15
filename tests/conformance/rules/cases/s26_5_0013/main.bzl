//! case: s26_5_0013
//! doc: spec
//! section: 26.5
//! title: a default-internal extension block is invisible from an importing module
//! rule: The compiler considers only extension members visible at the call
//!   site (§26.3's effective-visibility rule); an extension block is `public`
//!   or `internal`, default `internal` (§26.1). `Lib`'s block carries no
//!   `public` keyword, so it is internal to `Lib` and not a candidate for
//!   `App`'s call, even though `App` imports `Lib`.
//! cites: 26.5, 26.3, 26.1
//! expect: reject
//! diagnostic: Doubled
module App;
import Lib;

public class Program
  public static method Main()
    using var w := new Widget(6);
    Console.WriteLine(w.Doubled());
  end Main;
end Program;
