# n10-heap

*Area:* generics

A generic binary min-heap `MinHeap<T> where T: IComparable<T>` with `Push`, `Pop`, `PeekOrNull`, `Count`. Push 5 3 8 1 9 2, pop all: `1 2 3 5 8 9`. Then a heap of `Job(Priority: int, Name: string)` records implementing `IComparable<Job>` by priority then name: push (2,b) (1,z) (2,a) (0,q); pop all: `q z a b`. `PeekOrNull` on the empty heap prints `empty`.
