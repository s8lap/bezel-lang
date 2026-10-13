//! case: s14_2_0076
//! doc: spec
//! section: 14.2
//! title: where T: class meets OwningArray's class element requirement
//! rule: Non-nullable reference types only — classes (including Array<T>,
//!   OwningArray<T>, and other heap collection types), interfaces, string,
//!   function types.
//! cites: 14.2, 14.4, 26.4
//! expect: run
//! output: ok
//! issue: 5549
module Conf;
extension OwningArray<T> where T: class
  public method P(): int return 7; end P;
end;
class C public static method Main(): int Console.WriteLine("ok"); return 0; end Main; end C;
