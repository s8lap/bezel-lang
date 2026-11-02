//! case: s26_5_0061
//! doc: spec
//! section: 26.5
//! title: two equally-specific extension blocks on an Array<int> receiver are ambiguous
//! rule: If two equally-specific extensions match, the call is a compile error.
//! cites: 26.5, 26.5.1
//! expect: reject
//! diagnostic: equally specific
module Conf;

extension Array<int>
  public method Tag(n: int): int => n;
end extension;

extension Array<T>
  public method Tag(n: T): T => n;
end extension;

public class Program
  public static method Main(): int
    var a: Array<int> := [1, 2];
    Console.WriteLine(a.Tag(5).ToString());
    return 0;
  end Main;
end Program;
