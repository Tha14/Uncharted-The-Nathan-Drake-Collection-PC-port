# Claude Code PreToolUse hook: refuses writing, editing or creating Python files (this project is C++ only).
# Reads the tool call as JSON on stdin; exit code 2 blocks the call and shows the message to the agent.
# -Within <text>: only act when the file path, or the command or its working folder, contains that text
# (used by a user-wide copy of this hook that should only guard one project folder).
param([string]$Within = '')
$ErrorActionPreference = 'Stop'
try { $call = [Console]::In.ReadToEnd() | ConvertFrom-Json } catch { exit 0 }
$tool = [string]$call.tool_name
$ti = $call.tool_input
$msg = 'Blocked: this project is C++ only (see AGENTS.md). Do not write, edit or create Python. Write the change in C++; to replace a Python tool, port it to C++ and delete the .py in its own commit.'
$pyPath = '\.(py|pyw|pyi|ipynb)$'
function InScope([string]$text) {
    if (-not $Within) { return $true }
    return ($text.Replace('\', '/')).ToLower().Contains($Within.Replace('\', '/').ToLower())
}

if ($tool -in @('Write', 'Edit', 'MultiEdit', 'NotebookEdit')) {
    $path = [string]$ti.file_path
    if (-not $path) { $path = [string]$ti.notebook_path }
    if ($path -match $pyPath -and (InScope $path)) { [Console]::Error.WriteLine($msg); exit 2 }
}
elseif ($tool -in @('Bash', 'PowerShell')) {
    $cmd = [string]$ti.command
    # output redirected, copied, moved or written into a .py file, or a new file named *.py
    $writes = '(>>?|\btee\b|\bcp\b|\bmv\b|Copy-Item|Move-Item|Set-Content|Add-Content|Out-File|New-Item)[^|;&\r\n]*\.(py|pyw|pyi)\b'
    if ($cmd -match $writes -and ((InScope $cmd) -or (InScope ([string]$call.cwd)))) { [Console]::Error.WriteLine($msg); exit 2 }
}
exit 0
