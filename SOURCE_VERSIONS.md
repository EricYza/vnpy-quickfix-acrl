# Source versions and provenance

Snapshot prepared on 2026-09-02 before Git initialization.

| Component | Source | Base revision | Snapshot state |
|---|---|---|---|
| QuickFIX | `https://github.com/quickfix/quickfix.git` plus local optimization commits | `45a960be3fc2e3b24c4999e8857d30a7be101f0b` | Clean source tree at copy time |
| RLTE | `https://github.com/moritzweiss/rlte.git` | `1d0f5d3a096ca4616d6114c5855156a862a51fbf` | Includes current modified and untracked training/deployment worktree files |
| vn.py | `https://github.com/vnpy/vnpy.git` | `1b78494979deb4c4996f6b864f234d9839f2f239` | Clean source tree at copy time |
| vnpy QuickFIX gateway | Local integration package | No standalone Git history | Complete source and tests; runtime files excluded |
| vnpy AC-RL execution | Local integration package | No standalone Git history | Complete source and tests; runtime files excluded |

## Deployed model

```text
rlte/models/strategic_20_seed_0_eval_seed_100_eval_episodes_1000_num_iterations_500_bsize_400_ppo_logistic_normal_comparison_200k.pt
SHA-256: 7c108c25ab2f85335e87fc3bd85de51f0774128341784223ff593557fc645cf6
```

The snapshot keeps this 208 KiB deployment model and its compact evaluation
results. Other checkpoints, TensorBoard output, caches, generated QuickFIX
binaries, build directories, FIX stores/logs, and TLS private material are not
source dependencies and were excluded.

Local absolute symlinks to compiled QuickFIX tools and the unused external
pollnet checkout were also excluded. The current QuickFIX source has no build
reference to pollnet; its direct socket implementation lives under
`QuickFIX/src/C++/detail/`.

## Third-party notices

QuickFIX is distributed under the QuickFIX Software License 1.0; see
`QuickFIX/LICENSE`. This product includes software developed by
quickfixengine.org.

vn.py is distributed under the MIT License; see `vnpy/LICENSE`.

The copied RLTE upstream currently has no license file. Source availability does
not itself grant public redistribution rights. Confirm permission before making
the repository public.
