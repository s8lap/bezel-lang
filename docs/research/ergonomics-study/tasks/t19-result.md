# t19-result

*Area:* errors

A pipeline using a Result type: parse a string to int, then check it is even, then compute its half. Each step can fail with a message. For inputs `"8"`, `"7"`, `"x"` print `8 -> 4`, `7 -> error: 7 is odd`, `x -> error: not a number: x`.
