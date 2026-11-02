//! case: s26_5_0061
//! doc: spec
//! section: 26.5
//! title: two equally-specific extension blocks on unrelated interface heads
//!   are ambiguous
//! rule: If two equally-specific extensions match, the call is a compile error.
//! cites: 26.5, 26.5.1
//! expect: reject
//! diagnostic: equally specific
module Conf;

public interface IMark<T> end IMark;

public interface ITag<T> end ITag;

public class Carrier: IMark<int>, ITag<int>
  public constructor()
  end constructor;
end Carrier;

extension IMark<T>
  public method Tag(n: T): T => n;
end extension;

extension ITag<T>
  public method Tag(n: T): T => n;
end extension;

public class Program
  public static method Main(): int
    var a := new Carrier();
    Console.WriteLine(a.Tag(5).ToString());
    return 0;
  end Main;
end Program;
