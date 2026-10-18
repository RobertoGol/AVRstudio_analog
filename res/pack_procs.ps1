$ErrorActionPreference = 'Stop'
# Сборка res/procpack.bin: все профили Procs/*.proc в один blob для встраивания ресурсом.
# Формат: u32 nFiles, затем повторяется { u16 nameLen, name[ASCII], u32 dataLen, data }
$root = 'I:\МПИУС\Лабы\dld'
$procs = Get-ChildItem "$root\Procs\*.proc" | Sort-Object Name
if ($procs.Count -eq 0) { throw 'нет .proc-файлов' }
$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter $ms
$bw.Write([UInt32]$procs.Count)
foreach ($f in $procs) {
    $name = [System.Text.Encoding]::ASCII.GetBytes($f.Name)
    $data = [System.IO.File]::ReadAllBytes($f.FullName)
    $bw.Write([UInt16]$name.Length)
    $bw.Write($name)
    $bw.Write([UInt32]$data.Length)
    $bw.Write($data)
}
$bw.Flush()
[System.IO.Directory]::CreateDirectory("$root\res") | Out-Null
[System.IO.File]::WriteAllBytes("$root\res\procpack.bin", $ms.ToArray())
$bw.Close(); $ms.Close()
Write-Output ("procpack.bin: " + $procs.Count + " профилей, " + (Get-Item "$root\res\procpack.bin").Length + " байт")
