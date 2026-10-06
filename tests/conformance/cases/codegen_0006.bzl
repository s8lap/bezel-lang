// declarations
var balance: decimal;                       // typed, no initializer
var greeting := "Hello";                    // inferred
const Pi: double := 3.14159;                // single-assignment binding (§21.1)
using var _ := mutex.Acquire();             // anonymous owning binding; `await using var` in async bodies
record Row(Name: string, Age: int);         // LocalTypeDecl: `record` and `enum` ONLY, no visibility

// assignment / discard / increment / expression
total := total + 1;
total += 1;   total mod= 3;   cache ??= new Cache();   // := += -= *= /= mod= &= |= ^= <<= >>= ??=
_ := ComputeChecksum();                     // discard statement
i++;   i--;                                 // postfix, statement position only
Console.WriteLine("hi");

// if
if amount > 0 then
  Balance := Balance + amount;
elif amount = 0 then
  Log("zero");
else
  Reject();
end if;

// while, for (every shape)
scan: while i < 10 do  i := i + 1;  end scan;
for var i in 1..10 do ... end;                for var i in 0..<n step 2 do ... end;
for var x: int in source do ... end;          // optional ascription; ✗ `for var _: T in ...`
outer: for var row in rows do ... end outer;
await for var line in reader do ... end;      // `with token` supplies a CancellationToken (§30.7)
do  using var t := Open();  end;            // a `do` block ends a scope early

// case / when statements
case delta of
  -10..-6 => "strongly down",
  ..<0    => Log("down"),                   // single-statement arm
  0       => do Reset(); Log("flat"); end,  // do-block arm
  _       => Log("up")
end case;

if score >= 90 then Console.WriteLine("A") else Console.WriteLine("F") end if;

// return / throw / break / continue / yield
return;
return expression;
throw new ArgumentException("nope");
throw;                                      // bare re-raise, inside a catch arm body only
break;   break scan;
continue;   continue scan;
yield return i * i;
yield break;

// try — needs at least one of catch/finally; arms are COMMA-separated
try
  DoWork();
catch
  var ex: ArgumentException when ex.ParamName = "amount" => Log("bad amount"),
  InvalidOperationException => Retry(),
  var ex: IOException | TimeoutException => Retry(ex),
finally
  Log("done");
end try;
