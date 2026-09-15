//! case: l4_1_0003
//! doc: stdlib
//! section: 4.1
//! title: IsReadOnly is false on a mutable borrowing collection
//! rule: IsReadOnly is false for every standard collection implementing
//!   ICollection<T> (Array<T>, LinkedList<T>, Set<T>, Dictionary<K, V>).
//! expect: run
//! stdout: False
module Conf;

public class Program
  public static method Main()
    using var arr := new Array<int>();
    Console.WriteLine(arr.IsReadOnly);
  end Main;
end Program;
