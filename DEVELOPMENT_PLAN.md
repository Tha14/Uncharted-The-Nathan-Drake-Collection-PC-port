# Development Plan

Restore *Uncharted 2: Among Thieves* and *Uncharted 3: Drake’s Deception* multiplayer, with crossplay as the goal for PC, PS3, PS4, and PS5.

[README](README.md) · [Contributing](CONTRIBUTING.md)

Edit this file on GitHub and change `- [ ]` to `- [x]` as you complete each task.

## 1. Build on the existing launcher

- [ ] Evolve the existing launcher to support this project’s multiplayer features and give it its own identity.
- [ ] Support importing assets from players’ own PS3 copies so the launcher can convert the required multiplayer assets locally.
- [ ] Let players update the launcher, the multiplayer components, or both together.
- [ ] Evaluate Epic Online Services peer-to-peer networking as a crossplay option.
- [ ] Move multiplayer networking into the launcher, replacing the current RPCN integration inside Uncharted 3.

## 2. Finish Uncharted 3 multiplayer on PS4

- [ ] Fix the remaining gameplay and visual issues.
- [ ] Finish converting the remaining multiplayer assets from PS3 to PS4.
- [ ] Integrate the updated asset converter and patch manager into the launcher.

## 3. Port Uncharted 2 multiplayer to PS4

- [ ] Fix gameplay and visual issues in the port.
- [ ] Convert the remaining multiplayer assets from PS3 to PS4.
- [ ] Integrate the updated asset converter and patch manager into the launcher.

## 4. Package the multiplayer ports for PS4 and PS5

- [ ] Build a converter that adapts the PC multiplayer work into PKG packages for PS4 and PS5.
- [ ] Verify that the resulting builds can join cross-platform matches.

## 5. Bring PS3 into crossplay

- [ ] Create a PS3 PKG that replaces the original multiplayer networking with the shared peer-to-peer implementation.
- [ ] Add a way for PS3 players to sign in from within the game.
- [ ] Verify that PS3 players can join matches with PC, PS4, and PS5 players.

## 6. Support Linux and macOS

- [ ] Get the launcher running and the games playable on Linux.
- [ ] Get the launcher running and the games playable on macOS.
