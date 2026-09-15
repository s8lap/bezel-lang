//! case: s25_4_0001
//! doc: spec
//! section: 25.4
//! title: there is no user-defined conversion between record types
//! rule: User-defined types cannot declare implicit or explicit conversion
//!   operators; the explicit-cast expression `T(value)` applies only to the
//!   built-in conversions enumerated in §5.
//! cites: 25.4, 5.2
//! expect: reject
//! diagnostic: conversion
module Conf;

public record Dto(Id: int);

public record User(Id: int);

public class Program
  public static method Main()
    var d := new Dto(1);
    var u := User(d);
    Console.WriteLine(u.Id);
  end Main;
end Program;
