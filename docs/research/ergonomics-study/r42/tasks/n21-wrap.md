# n21-wrap

*Area:* text

Word-wrap a paragraph to a width of 28 characters (greedy; words longer than the width are put on their own line), print each line between `|` bars padded to the width, and print a centered title `[ Wrap demo ]` padded with `=` to the width (`PadCenter`; the odd extra `=` goes to the right, stdlib §9.6): `=======[ Wrap demo ]========`. Text: `The quick brown fox jumps over the lazy dog while supercalifragilistic words test the edge cases.` Expected lines (after the title): `|The quick brown fox jumps   |`, `|over the lazy dog while     |`, `|supercalifragilistic words  |`, `|test the edge cases.        |`.
