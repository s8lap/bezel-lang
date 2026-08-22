for var t in transactions do Process(t); end;
var first: Row? := null;
if rows <> null then first := rows[0]; end;   // §7.4 narrows a LOCAL; `rows?.Count` also works
var xs: Array<int> := [10, 20, 30];       ProcessBuffer([1, 2, 3]);
var max := (if a > b then a else b);
c++;    count := count + 1;
if count <> 0 then ... end;
case point of Point(var a, var b) => Use(a, b) end;
public method Log(items: Array<string>);     Log(["a", "b", "c"]);
var c := new Config(timeout := 30, retries := 3);             // named constructor arguments
