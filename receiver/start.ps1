param(
    [string]$Vault = '',  # empty: use %APPDATA%/KnotNotes/config.json
    [string]$Address = ''
)
$ErrorActionPreference = 'Stop'
$pythonPath = Join-Path $PSScriptRoot '../.venv/Scripts/python.exe'
if (-not (Test-Path -LiteralPath $pythonPath)) { throw '请先建立项目 .venv 并安装 receiver/requirements.txt。' }
$receiverArgs = @((Join-Path $PSScriptRoot 'knot_receiver.py'))
if ($Vault) { $receiverArgs += @('--vault', $Vault) }
if ($Address) { $receiverArgs += @('--address', $Address) }
& $pythonPath @receiverArgs
exit $LASTEXITCODE
