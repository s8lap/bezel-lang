# t26-plugins

*Area:* generics & interfaces

A plugin registry: interface ICommand with Name and Run(args) returning an exit code. Register `echo` (prints its args joined by spaces) and `count` (prints the number of args). Dispatch a list of command lines: `echo hello world` prints `hello world`; `count a b c` prints `3`; `nope x` prints `unknown command: nope`. Print `registered: count, echo` (sorted).
