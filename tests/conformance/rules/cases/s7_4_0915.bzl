//! case: s7_4_0915
//! doc: spec
//! section: 7.4
//! title: a ReadOnlyStringSpan? mutable field is not narrowed by a null test on the field
//! rule: Accessing a nullable *mutable* field without first copying it to a local is a compile error when
//!   the access requires non-null.
//! cites: 7.4
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  private _s: ReadOnlyStringSpan?;

  public constructor()
    this._s := null;
  end constructor;

  method Run()
    if this._s <> null then
      Console.WriteLine(this._s.Length);
    end if;
  end Run;

  public static method Main()
    var p := new Program();
    p.Run();
  end Main;
end Program;
