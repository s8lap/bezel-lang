//! case: s8_4_9861
//! doc: spec
//! section: 8.4
//! title: Object.ReferenceEquals takes a record, enum, choice and interface value and compares box identity
//! rule: a non-null value-typed argument produces a fresh box and compares unequal to every other argument
//! cites: 8.1, 8.4, 8.6, 26.5, 35.8
//! expect: run
//! warns: yes
//! output: False
//!   False
//!   False
//!   True
//! notes: The accepting twin of s26_5_9860-s26_5_9863 (harvested from the #9021 review): the same four
//!   kinds, passed to the root's static through the name `Object`. A record, an enum and a value-like
//!   choice are boxed afresh at each argument (8.6, 35.8), so each compare is False, and 8.4's
//!   value-typed-argument warning is drawn; the interface-typed null compares True with itself.
module Conf;

public record R(X: int);
public enum E A, B; end E;
public choice Ch Left(v: int), Right; end Ch;
public interface I end I;

public class Program
  public static method Main(): int
    var r := new R(1);
    var i: I? := null;
    Console.WriteLine(Object.ReferenceEquals(r, r));
    Console.WriteLine(Object.ReferenceEquals(E.A, E.B));
    Console.WriteLine(Object.ReferenceEquals(Ch.Right, Ch.Right));
    Console.WriteLine(Object.ReferenceEquals(i, i));
    return 0;
  end Main;
end Program;
