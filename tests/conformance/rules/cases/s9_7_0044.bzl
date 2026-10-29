//! case: s9_7_0044
//! doc: spec
//! section: 9.7
//! title: a record declared in a method body may not implement IDisposable
//! rule: Language spec §9.7 (v0.350): "Records cannot have destructors, `owned` fields, or
//!   implement `IDisposable` — directly or through an interface extending it, `IAsyncDisposable`
//!   among them." §21.10 (v0.350) makes a local record "an ordinary nominal record or enum": "every
//!   §9 (records) and §10 (enums) rule applies unchanged".
//! cites: 9.7, 21.10
//! expect: reject
//! diagnostic: IDisposable
//! issue: 7490
//! notes: the local-record position of s9_7_0043 (nested in a class). Its accepting twin is
//!   s21_10_0001, a local record with the same body and no disposal interface. Harvested from
//!   the R36 reviewer probe nested_record_method_local.
module Conf;

public class Program
  public static method Main(): int
    record R: IDisposable
      public _x: int := 0;
    end R;
    Console.WriteLine("hi");
    return 0;
  end Main;
end Program;
