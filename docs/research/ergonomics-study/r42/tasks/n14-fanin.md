# n14-fanin

*Area:* async

Three producer tasks each send 1..5 multiplied by their id (1, 10, 100) into one bounded channel (capacity 4) through their own sender; the consumer drains `ReceiveAll` and sums. Print `received 15 items, sum 1665`. Producers are started as named tasks and settled with `Task.WhenAll`.
