//! case: s11_6_0020
//! doc: spec
//! section: 11.6
//! title: an enum nested in a generic type is reached through a chain head supplying the arguments
//! rule: A chain head that supplies the enclosing type's arguments (§2.7 rule
//!   1, `Tree<int>.Node<int>.Create()`) reaches that one type whatever the
//!   arguments ... the rest of the chain resolves against that type's
//!   members, declared or inherited (a nested type, an enum constant, a
//!   static member — each segment in turn
//! cites: 2.7, 11.6
//! expect: run
//! stdout: 1
module Conf;

public class C<T>
  public enum Color Red, Green; end Color;
end C;

public class Program
  public static method Main(): int
    var c := C<int>.Color.Red;
    if c = C<int>.Color.Red then Console.WriteLine(1); end if;
    return 0;
  end Main;
end Program;
