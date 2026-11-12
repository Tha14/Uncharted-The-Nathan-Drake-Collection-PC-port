@AGENTS.md

Claude Code: the hook in `.claude/settings.json` refuses any write or edit of a Python file here.
Do not work around it; write the change in C++ instead.

Keep ONLY the latest version of anything versioned (see AGENTS.md, "Versions"): never copy a folder to make a new
version, `git mv` the old one and change its files. Older versions live in git history only.

Avoid overly complex implementations that can be simplified (see AGENTS.md, "Keep it simple").
