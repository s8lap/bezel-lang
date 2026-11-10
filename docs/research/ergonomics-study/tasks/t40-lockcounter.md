# t40-lockcounter

*Area:* resources & concurrency

Four domains each increment a shared counter 1000 times; the counter is protected by a mutex (or the language's lock primitive) shared across the domains; the main program waits for all and prints `count 4000`. Then the same with an atomic integer, printing `atomic 4000`.
