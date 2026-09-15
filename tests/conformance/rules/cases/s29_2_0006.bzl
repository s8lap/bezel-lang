//! case: s29_2_0006
//! doc: spec
//! section: 29.2
//! title: a fresh iterable-family for-loop source is disposed by the desugaring's hidden using var
//! rule: When `source` is a fresh class-typed expression ... the wrapping
//!   `using var` assumes the disposal obligation and ensures the iterable is
//!   disposed when the loop ends.
//! cites: 29.2
//! expect: run
//! stdout: disposed
module Conf;

public class LoudEnumerator: IEnumerator<int>
  private _i: int := -1;

  public constructor()
  end constructor;

  public method MoveNext(): bool
    _i := _i + 1;
    return _i < 1;
  end MoveNext;

  public property Current: int => _i;
end LoudEnumerator;

public class Loud: IEnumerable<int>
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("disposed");
  end destructor;

  public method GetEnumerator(): owned IEnumerator<int>
    return new LoudEnumerator();
  end GetEnumerator;
end Loud;

public class Program
  public static method Main()
    for var x in new Loud() do
      _ := x;
    end;
    Console.WriteLine("after");
  end Main;
end Program;
