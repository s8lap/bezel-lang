//! case: s8_1_0006
//! doc: spec
//! section: 8.1
//! title: a user-declared generic `Object<T>` is a user type, not the root
//! rule: Every other declaration of the name is ordinary: a generic `Object<T>` is a different name ([§2.2](#sec-imports))
//! cites: 8.1, 2.2
//! expect: run
//! output: 7
//! issue: 7992
module Conf;

public class Object<T>
  public method Hi(): int => 7;
end Object;

public class Program
  public static method Main(): int
    using var o := new Object<int>();
    Console.WriteLine(o.Hi());
    return 0;
  end Main;
end Program;
