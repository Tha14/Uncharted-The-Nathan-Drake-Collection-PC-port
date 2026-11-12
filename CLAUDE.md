@AGENTS.md

Claude Code: the hook in `.claude/settings.json` refuses any write or edit of a Python file here.
Do not work around it; write the change in C++ instead.

Keep ONLY the latest version of anything versioned (see AGENTS.md, "Versions"): never copy a folder to make a new
version, `git mv` the old one and change its files. Older versions live in git history only.

Keep it simple, but always legal (reimplement game code from a description, never rewrite it 1:1) and only
simplify when the result does 100% of what the original does; if a simpler way would break anything, don't (see
AGENTS.md, "Keep it simple, but legal and exact").

Port assets from the PS3 only when PS4 has none: if PS4 has the asset anywhere (campaign included, e.g.
baseball-shirt / red shirt Drake as a campaign skin), use PS4's, with its better textures and a smaller import
(see AGENTS.md, "Assets: PS4 first").

Helper source (`u3mp/helper-source/`), in short: a contributor syncs their fork, edits only that folder, and opens a
pull request; the maintainer merges it, then builds and ships the next helper version. So: never make or rename a helper
version, never touch the generated block at the end of `u3_lan_profiles.c`, write only between the contributor markers,
and port only the rows assigned to you (see AGENTS.md, "Helper source: several people at once", and CONTRIBUTING.md,
section 7).
