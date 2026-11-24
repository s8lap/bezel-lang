//! case: s28_14_8601
//! doc: spec
//! section: 28.14
//! title: an imported indexer called with a function read from a field leaves the field intact
//! rule: the callee receives the callback for the duration of the call only
//! cites: 28.14, 28.9
//! expect: run
//! stdout: 400
//! notes: Harvested from the #9083 review (probe A_get_field with Lib.bzl trimmed to the two classes
//!   used). The indexer parameter q is call-bounded: the imported getter only invokes it, so the
//!   function value read from h.F stays held by the field, and h.F() afterwards still works. Per
//!   iteration fl[h.F] = 1 and h.F() = 1, so 200 iterations print 400. An accepting control
//!   (green on the base tree): it pins that the callee's release of a lambda argument does not
//!   also release a function value that a field still holds.
module A_get_field;
import System;
import Lib;
public class Program
  public static method Main(): int
    var t := 0;
    var h := new Lib.Holder();
    var fl := new Lib.Fl();
    for var j in 0..<200 do
      t := t + fl[h.F] + h.F();
    end;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
