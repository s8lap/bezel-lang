# n15-deadline

*Area:* async

Run five lookups concurrently, each an async method `Lookup(id, token)` delaying `id*40` ms, with a whole-batch deadline of 140 ms (`WithCancelAfter`). Collect which finished: print `finished: 1 2 3` and `cancelled: 4 5`. Each lookup catches nothing itself; the batch awaits each named task and catches `OperationCanceledException` per task (an expected outcome at that call).
