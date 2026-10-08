42   0xFF   0b1010_1010   1_234_567   42L   42U   42UL
3.14   3.14f   1.23e-4   1.23456789m   1.23e2m
'a'  '\n'  '\u0041'  '\U0001F600'
"Hello\n"    @"C:\raw\path"                       // verbatim; "" is one embedded quote
$"Hello, {name}!"     $@"C:\Users\{user}\x.pdf"
$"{amount,12:F2}"     $"|{name,20}|"   $"{ratio:P1}"   // {expr,align:format}
$"{{literal braces}}"
true   false   null   ()                          // bool / null / the unit value
[1, 2, 3]     []      ["a": 1, "b": 2]            // collection and dictionary literals
