
### Following directories contain source copied and modified from other projects
- `argmap/`: https://github.com/mpc-msri/EzPC
- `net/`: https://github.com/emp-toolkit
- `rapidjson/`: https://github.com/Tencent/rapidjson


### Pinned upstream sources

`SEAL/` and `faiss/` are vendored at the exact commits in `dependencies.json`,
previously recorded as gitlinks. Their upstream LICENSE files are retained.
No nested Git repositories are needed. The local Compass core uses its modified
implementation under `src/faiss/`; the optional upstream libraries are for
`COMPASS_BUILD_LEGACY_BASELINES=ON`.
