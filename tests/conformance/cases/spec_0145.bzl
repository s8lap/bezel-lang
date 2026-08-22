public class Logger  // declaration sketch; method bodies elided
  public method Log(level: int);
  public method Log(level: long);
end Logger;

var lg := new Logger();
var n: int := 10;

lg.Log(n);            // n is int: exact match — calls Log(int), not the int→long widening
lg.Log(42);           // 42 is an int literal: exact match — calls Log(int)
lg.Log(42L);          // 42L is a long literal: exact match — calls Log(long)
