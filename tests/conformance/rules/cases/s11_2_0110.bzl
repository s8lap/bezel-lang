//! case: s11_2_0110
//! doc: spec
//! section: 11.2
//! title: an indexer's element type substituted to int matches the implementation's int indexer
//! rule: A read-write member satisfies a read-only interface member of the same name and type: if IReadOnlyList<T> declares `readonly property [i: int]: T` and IList<T> declares `property [i: int]: T`, a single read-write indexer satisfies both.
//!   The implementation's indexer must therefore have the same type as the interface's indexer, which under I<int> is the substituted element type.
//!   Under I<int> the element type T is int, so `readonly property [i: int]: T` and the implementation's `property [i: int]: int` match.
//! cites: 11.2
//! expect: run
//! stdout: 8
module Conf;

public interface I<T>
  readonly property [i: int]: T;
end I;

public class K: I<int>
  public constructor() end constructor;

  public property [i: int]: int => i * 2;
end K;

public class C
  public static method Main(): int
    var s: I<int> := new K();
    Console.WriteLine(s[4]);
    return 0;
  end Main;
end C;
