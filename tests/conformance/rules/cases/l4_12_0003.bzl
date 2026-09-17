//! case: l4_12_0003
//! doc: stdlib
//! section: 4.12
//! title: GetEnumerator() returns an owned IEnumerator<T> the caller must dispose
//! rule: GetEnumerator(): owned IEnumerator<T>; -- the caller takes ownership of
//!   the returned enumerator and must dispose it.
//! cites: 4.12, 28.3
//! expect: run
//! stdout: closed
module Conf;

public class LoudEnumerator: IEnumerator<int>
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("closed");
  end destructor;

  public method MoveNext(): bool => false;
  public property Current: int => 0;
end LoudEnumerator;

public class Source: IEnumerable<int>
  public constructor()
  end constructor;

  public method GetEnumerator(): owned IEnumerator<int>
    return new LoudEnumerator();
  end GetEnumerator;
end Source;

public class Program
  public static method Main()
    do
      using var src := new Source();
      using var e := src.GetEnumerator();
    end;
  end Main;
end Program;
