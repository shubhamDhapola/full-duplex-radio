# Contributing

## Building

```bash
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires CMake ≥ 3.24, Ninja, and a C++20 compiler. Catch2 and nlohmann/json are
fetched at configure time and are test-only; `radio_core` itself has no
dependencies.

## Before opening a pull request

```bash
# 1. Formatting, using the pinned version CI uses
pip install clang-format==20.1.7
clang-format -i $(git ls-files '*.cpp' '*.hpp')

# 2. Tests, warning-free
cmake --build build && ctest --test-dir build --output-on-failure

# 3. Sanitizers — several tests are meaningless without them
cmake -B build-san -G Ninja -DRADIO_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-san && ctest --test-dir build-san
```

The build treats a broad warning set as significant, including `-Wconversion`,
`-Wsign-conversion` and `-Wold-style-cast`. These are not cosmetic: a silently
narrowed length is how a bounds check gets bypassed, and a C-style cast can
silently mean `reinterpret_cast`. Do not suppress a warning without saying why
in a comment.

## Code conventions

- **No allocation on any path that runs per packet or per audio frame.** Use
  fixed-size members or a pool allocated at startup.
- **No blocking on those paths either** — no mutex, no file I/O, no name
  resolution.
- **`noexcept` throughout the core.** Errors are returned as values, because
  unwinding may allocate.
- **Comments explain why, not what.** The diff already shows what changed.
- Return a classified error rather than a boolean where the caller could act
  differently on the reason.

## Changing the wire format

The protocol is defined normatively in
[protocol/specification.md](protocol/specification.md). Any change to it:

1. Update the specification first. It is the source of truth; the code is an
   implementation of it.
2. Update `protocol/testvectors/generate.py`, which is a **separate**
   implementation written from the specification rather than from the C++
   encoder. Keeping the two independent is what makes them evidence of anything.
3. Regenerate and commit the vectors:
   ```bash
   python3 protocol/testvectors/generate.py
   ```
   CI regenerates them and fails if the committed file differs.
4. Update the C++ codec and confirm the conformance tests pass.

Additive changes should consume reserved flag bits or unassigned type codes. Any
change that alters the meaning of an existing field requires a version bump,
because a receiver drops unrecognised versions wholesale rather than attempting
a partial parse.

## Testing expectations

New code is expected to carry tests from at least the relevant families:

| Family | When it applies |
|---|---|
| Unit, against hand-computed values | always |
| Conformance vectors | any wire-format change |
| Adversarial input | anything parsing untrusted bytes |
| Negative control | any mitigation, guard or workaround |

The last one is easy to skip and worth the most. A test asserting that the
problem *occurs without* the fix is what stops the fix being deleted later as
unnecessary complexity — every positive test would still pass.

Where a value is approximate, assert its documented error bound rather than an
exact figure, and do not weaken an implementation to satisfy a test.

## Commit messages

Conventional Commits subject, then a short paragraph of context, then numbered
points — one per distinct change or decision, each led by a short bold label.

```
type(scope): imperative subject, lower case, no trailing period

One to three sentences on what was wrong or why this exists.

1. **Short label.** What changed and why, naming the file or function where
   that aids review.
2. **Next point.** Grouped by concern, not by file.
3. **Tests.** Called out explicitly when present.
```

Types: `feat`, `fix`, `refactor`, `perf`, `test`, `docs`, `build`, `ci`, `chore`.
Wrap at 72 columns. One concern per commit — if the points do not share a
subject line, it should have been two commits.
