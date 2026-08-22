foreach var t in transactions do Process(t); end foreach;   // WRONG: no such keyword
var first := rows?[0];                                      // WRONG: there is no `?[` suffix
var xs: Array := [10, 20, 30];                              // WRONG: `Array` is not a type
var ys := [1, 2, 3];                                        // WRONG: no target type for the literal
var max := a > b ? a : b;                                   // WRONG: no `?:` operator
x := y++;   ++c;   if count++ > 3 then ... end;               // WRONG: `++` is a postfix STATEMENT
if count then ... end;                                        // WRONG: no truthiness
var (a, b) := point;                                        // WRONG: destructuring is pattern-position only
public method Log(params items: Array<string>);             // WRONG: no `params`
var c := new Config() with(Timeout := 30);                  // WRONG: `with` needs a RECORD source
