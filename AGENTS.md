# Notes for coding agents

**hdslave Psychic** is a Labyrinth-style generative synth with two bit-shifting sequencers: a CLAP plug-in for the Akai MPC Sample. It is made to run in hdslaveOS, the hdslave fork of Kerem Kiliç's Melodee. Its CLAP id is `com.hdslave.psychic`, and it builds to `build/Psychic.clap`.

## Read the workspace guide first

On the author's machine, this repository sits in one folder beside the other hdslave projects. That folder's `../AGENTS.md` and `../Workflow/` hold the shared workflow:
- building in the Linux VM;
- what hdslaveOS does to plug-ins;
- screen pages;
- reviews;
- publishing, the hub repository and staging for the SD card.

Read `../AGENTS.md`, then `../Workflow/plugins.md` and `../Workflow/host-facts.md`, before you change anything. The rules below hold either way.

## Build and test

- `./build.sh` builds the plug-in in a Linux arm64 container (image `melodee-builder`, Colima VM `melodee`). It runs `make all test`: the host tests in `tests/host.cpp` and the sound's fingerprint. Pass make targets to run others.
- `tools/bench.sh` counts the instructions of a 128-frame block with callgrind. A block is about 4.8 M cycles on one of the MPC's Cortex-A55 cores.
- Check the build's own exit code: `./build.sh | grep …` hides a failure.

## Rules

- **Fixed for saved projects.** Parameter ids, preset keys and choice orders never change once released, because saved projects and presets refer to them.
  - Add new ones at the end.
  - Ranges stay as they are too: the host stores bindings normalized to the range.
- **The sound is held to the bit** by `tests/fingerprint.txt`.
  - A refactor or speed-up must leave every hash unchanged. With `-ffp-contract=fast`, even reshaping a loop can change the last bit, so check after every edit.
  - Only a change meant to change the sound runs `make fingerprint-update`, and its commit says which hashes moved and why.
- **Real-time and determinism.** The output is the same at every block size. No allocation, locks or file I/O on the audio thread.
- **Notes** must survive how hdslaveOS sends them:
  - every MIDI event of a block arrives at frame 0;
  - a re-pressed key arrives as on, on, off;
  - CC 120 and 123 arrive at 0 on every channel when the host silences.
- **Commits** are made as `hdslave <111106843+hdslave@users.noreply.github.com>`, with Conventional Commits. Never push, publish or stage for the card without the owner's word.
- **Other agents' work.** Others may be working here. Check `git status` and `git worktree list`, and never edit, commit or reset files you did not change.
- **Names and art.** No trademarked names in the plug-in's names, presets, UI text or art. Art is original; fonts are OFL, with their licences in `licenses/`.
- **Read the README** before you change the sound. It lists the sources, the inferred values and the deliberate deviations. Keep it current: version, costs, and "not yet run on the MPC" until it has been.

## This plug-in

- The engine changes some parameters itself and reports them to the host as output events: Bits 1/2 and Seq 1/2 CV 1-8 (`Engine::takeChange`). The fingerprint hashes those reported values too.
- The Save/Recall buffer is a `BUFR` trailer in the state.
- The fingerprint's random scenarios draw for a fixed parameter list (`order01`). Add a new list when adding parameters.
- **Pads.** The .clap embeds Melodee's drum-map JSON (`padMapJson` in `src/psychic.cpp`, kept by `padsJson()`), so hdslaveOS plays a Psychic track's pads as drums: A01-A16 send 36-51 whatever the octave, key or scale, and - / + move the bank 16 keys. With Run on (playing or stopped), every key is a step pad (`Engine::preview`), read through `padOf` (`src/params.h`): pad = (key-36) mod 16, bank = floor((key-36)/16), clamped to -2..+3 octaves (banks A-D are 0-3 up). 36-43 are SEQ 1's bits, 44-51 SEQ 2's (chained: both CV outputs): the bit's CV plus the bank's octaves (`Lane::octaves`, held with the CV until the next on bit) and both EGs at the velocity, moving no head and reporting nothing. With Run off there are no banks: every key transposes by key - 60 (Unquantized too, `cvShift`) and strikes, to the bit as 0.2.6. Where a key played a pitch before, keep it playing exactly that. `laneShift` holds `cvShift` plus each lane's octaves and is kept current by `set()`, `note()`, `preview()` and `moveOn()`: working it out in `seqCv` or `retarget` makes the per-sample loop cost a fifth more.

## Release packaging

Every release ZIP includes the full repository `LICENSE.txt` beside the third-party notices, outside signed plug-in bundles. Preserve both when packaging the plug-in for another project.
