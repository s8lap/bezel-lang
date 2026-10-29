//! case: s21_1_0005
//! doc: spec
//! section: 21.1
//! title: a runtime const initialized from Guid.ToByteArray, which returns owned, is rejected
//! rule: The initializer of a runtime `const` must not be an **owned production**
//! cites: 21.1, 28.3, 2.6
//! expect: reject
//! diagnostic: owned
module Conf;

public class Program
  public static method Main()
    var g := Guid.NewGuid();
    const b := g.ToByteArray();
    Console.WriteLine(b.Count);
  end Main;
end Program;
