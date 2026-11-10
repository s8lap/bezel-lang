# t17-parsesum

*Area:* errors

Parse lines `key=value` where value should be an int. Lines: `a=1`, `b=x`, `c=3`, `d`, `e=-4`. Print an error line for each bad one (`line 2: bad number 'x'`, `line 4: missing '='`), then `sum=0` of the good values (1+3-4) and `good=3 bad=2`.
