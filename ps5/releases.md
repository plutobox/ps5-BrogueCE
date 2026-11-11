# Native PS5 builds and releases

The native workflow follows ps5-crawl and ps5-NetHack: builds on pushes to
`ps5` and pull requests, manual builds, and versioned releases. Each full
release contains one installable title ZIP. SHA256SUMS is retained alongside
the ZIP in the Actions artifact. Published release assets are never replaced.

The native app boilerplate is a Git submodule pinned to
`4f531c4b517f80bcb6b1267135848168d2250047`. It is not updated automatically.
OpenGL, SDL2 and the platform layer use pinned commits; SDK archives are
verified with SHA-256. Each repository owns its `contentVersion` and release
workflow. Updating the toolchain is a reviewed submodule/pin change.

## Build locally (Ubuntu 24.04 / WSL)

Install the packages listed in `.github/workflows/ps5-native.yml`, then:

```sh
git submodule update --init ps5/toolchain/boilerplate
bash ps5/setup-native.sh
source ps5/.deps/environment.sh
cd ps5
make -f Makefile.native -j"$(nproc)"
bash package-native.sh --build
```

Downloads and tools live in `ps5/.deps/` and the boilerplate's ignored
`.deps/`. This does not install SDKs in `/opt` or depend on sibling checkouts.
A failed SDL setup may leave its output directory behind; remove only
`ps5/.deps/ps5-opengl/build/sdl-native` before retrying it.

## Publish a full release

Merge the workflow onto the default branch to make its manual Run workflow
button available. Run **PS5 native title** on the **ps5** branch:

- `none`: build and upload an Actions artifact only.
- `current`: release the committed `contentVersion` (for the first release).
- `patch`, `minor`, `major`: raise that version before building.
- `notes`: optional Markdown release notes.

Only after a successful build does the workflow commit the version change,
push the branch and version tag atomically, and publish a full GitHub release.
The workflow token needs permission to push to `ps5`; branch protections can
prevent automatic version commits. Alternatively, bump `sce_sys/param.json`
manually, commit it, and push a tag matching `NN.NNN.NNN` exactly.
A manual build with `none` is the recommended first CI check.
