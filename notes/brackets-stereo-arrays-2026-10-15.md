# aither issues: brackets for stereo and for arrays

2026-10-15. This is item 1 in `lab:~/handover/HANDOVER-aither-sshmail-2026-10-14.md`. Nothing has been changed yet; it waits for Roland's decision.

## The rule today

The compiler decides "stereo" from shape alone: any 2-element array is an `[L, R]` pair, wherever it appears (`codegen.nim:493`, `isStereoReturn`: `of nkArr: n.kids.len == 2`). Arrays of every other length are tables or polyphony.

## What I found

Tested with the current engine (`aither-scope:next4`) in a throwaway container on lab.

| Patch | Result |
|---|---|
| `wave(1, [0.25, 0.75])` | **fails**: "stateful call 'wave' receives a stereo value" |
| `let t2 = [0.25, 0.75]` then `wave(1, t2)` | **fails**, same error |
| `osc(sin, [220, 330])` (two-voice polyphony) | **fails**, same error |
| `wave(1, [0.25, 0.75, 0.5])` | works |
| `t2[int(phasor(1)*2)]` (indexing a 2-element let) | works |
| `let p = [2.0, 3.0]` inside a def, then `p[x]` | works |
| `t2 * 0.1` | works, but as stereo (L and R, not a table times 0.1) |

So the problem is narrow. A 2-entry table works when you index it but not when you pass it whole to something that expects a table. For the same reason, two-voice polyphony is impossible.

I found two smaller bugs along the way:

- The error message always says "split L/R first via `bass[0]/bass[1]`", whatever your variable is called. The name `bass` is hard-coded in `codegen.nim:1790`.
- `let t3 = [a, b, c]` followed by `t3 * 0.1` crashes the C compile (`'l_t3' undeclared`). The spec says it should use the first element.

## Options

**A. Let the receiving function decide (my recommendation).** Keep `[a, b]` as the one syntax. Where a function expects a table (`wave`'s table argument, `osc`'s frequency for polyphony, and later def array parameters), it reads any array as a table, whatever its length. Everywhere else (arithmetic, pipes, play results), a 2-element array stays a stereo pair.

PHILOSOPHY.md already says this: "They become a stereo pair when a different operation reads them differently… The cells don't care; the operations do." Right now the compiler applies that by length instead of by operation. This option adds no new syntax and doesn't touch any existing patch.

**B. Give stereo its own syntax**, such as `stereo(l, r)`. That makes the meaning explicit, but it adds something to the core. There are about 180 two-element literals in 73 `.aither` files, mostly stereo returns, `pan` and `width` in stdlib, and `[s, s]` in sounds, so most of those would need rewriting.

**C. Give tables their own syntax**, such as `table(...)`. This is worse than B, because tables are the more common use.

One case A doesn't cover is `t2 * 0.1` on a 2-element table, which would still act as stereo. I think that's acceptable: scaling a whole table is rare, and `each` or indexing handles it.

## If A is chosen

It's a small change in codegen: the table-taking functions skip the stereo check for their table argument. Make it on lab along with the two small fixes, add tests for each row of the table above, and leave the live container alone until Roland swaps it between sets.
