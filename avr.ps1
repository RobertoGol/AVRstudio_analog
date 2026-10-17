# =====================================================================
#  AVR CLI Studio - Главное управление (оболочка)  |  avr.ps1
#  Замена меню AVR Studio 4: Project/Build/Debug/Tools -> CLI
#  Совместимость: PowerShell 1.1+ ... 7.6+ (README sec.10):
#  только базовые командлеты, Write-Host -ForegroundColor (без ANSI),
#  нет ConvertFrom-Json (в старых PS нет) - правка JSON строкой.
#  Запуск:  .\avr.ps1                 (интерактивный режим, меню)
#           .\avr.ps1 proc list       (одиночная команда)
# =====================================================================

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Root
$Ctos   = Join-Path $Root 'echo.ctos'
$Procs  = Join-Path $Root 'Procs'
$Props  = Join-Path $Root '.vscode\c_cpp_properties.json'
$Mkfile = Join-Path $Root 'Makefile'

# ---------------------------------------------------------------------
# Вспомогательные функции
# ---------------------------------------------------------------------
function Read-Ini() {
    # Возвращает hashtable: "Section|Key" = value
    $ini = @{}; $sec = ''
    if (-not (Test-Path $Ctos)) { return $ini }
    foreach ($ln in [IO.File]::ReadAllLines($Ctos, [Text.Encoding]::UTF8)) {
        $t = ('' + $ln).Trim()
        if ($t -eq '' -or $t.StartsWith('#')) { continue }
        if ($t.StartsWith('[') -and $t.EndsWith(']')) {
            $sec = $t.Trim('[', ']'); $secTrim = $t.Substring(1, $t.Length - 2); $sec = $secTrim.Trim(); continue
        }
        $p = $t.IndexOf('=')
        if ($p -gt 0) { $ini[($sec + '|' + $t.Substring(0, $p).Trim())] = $t.Substring($p+1).Trim() }
    }
    return $ini
}
function Get-Cs($sec, $key, $def) {
    $ini = Read-Ini
    $k = $sec + '|' + $key
    if ($ini.ContainsKey($k)) { return $ini[$k] }
    return $def
}
function Write-IniKey($sec, $key, $val) {
    # Переписывает echo.ctos, заменяя значение ключа (или добавляя в секцию)
    $lines = [IO.File]::ReadAllLines($Ctos, [Text.Encoding]::UTF8)
    $cur = ''; $found = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $raw = $lines[$i]
        $t = ($raw + '').Trim()
        if ($t.StartsWith('[') -and $t.EndsWith(']')) { $cur = $t.Trim('[', ']').Trim() }
        if ($cur -eq $sec -and $t.StartsWith($key, [System.StringComparison]::OrdinalIgnoreCase)) {
            $lines[$i] = $key + ' = ' + $val; $found = $true; break
        }
    }
    if (-not $found) {
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $t = ($lines[$i] + '').Trim()
            if ($t.StartsWith('[' + $sec + ']')) {
                $j = $i + 1
                while ($j -lt $lines.Count -and ($lines[$j] + '').Trim() -ne '' -and -not ($lines[$j].ToString()).Trim().StartsWith('[')) { $j++ }
                $newLine = $key + ' = ' + $val
                $lines = @($lines[0..($j-1)]) + @($newLine) + @($lines[$j..($lines.Count-1)])
                $found = $true; break
            }
        }
    }
    if ($found) { [IO.File]::WriteAllLines($Ctos, $lines, (New-Object System.Text.UTF8Encoding($false))) }
}

function Read-ProFile($path) {
    # hashtable "Section|Key" = value для .proc
    $ini = @{}; $sec = ''
    foreach ($ln in (Get-Content $path)) {
        $t = ('' + $ln).Trim()
        if ($t -eq '' -or $t.StartsWith('#')) { continue }
        if ($t.StartsWith('[')) { $sec = $t.Trim('[', ']').Trim(); continue }
        $p = $t.IndexOf('|')
        if ($p -gt 0) { $ini[($sec + '|' + $t.Substring(0, $p).Trim())] = $t.Substring($p+1).Trim(); continue }
        $p = $t.IndexOf('=')
        if ($p -gt 0) { $ini[($sec + '|' + $t.Substring(0, $p).Trim())] = $t.Substring($p+1).Trim() }
    }
    return $ini
}
function Parse-Tc($v) {
    # Addr/число: 0xNN, 8192, 0.15KB...
    if ($v -eq $null) { return 0 }
    $v = ('' + $v).Trim()
    if ($v.Contains('#')) { $v = $v.Split('#')[0] }
    $v = $v.Trim()
    if ($v.StartsWith('0x')) { return [Convert]::ToInt32($v.Substring(2), 16) }
    return [int](($v -replace '[^0-9-]', ''))
}

function Select-ProcImpl($name, $mhz) {
    $path = ''
    Get-ChildItem $Procs -Filter '*.proc' | ForEach-Object {
        $n = [IO.Path]::GetFileNameWithoutExtension($_.Name)
        if ($n -ieq $name -and $path -eq '') { $path = $_.FullName }
    }
    if ($path -eq '') {
        Write-Host ("  ! Profile '" + $name + "' not found in " + $Procs) -ForegroundColor Red
        return $false
    }
    $pf = Read-ProFile $path
    $chip  = $pf['Identity|Name'];        if ($chip -eq $null -or $chip -eq '') {
        Get-Content $path | Out-Null
        $chip = [IO.Path]::GetFileNameWithoutExtension($path) }
    $mcut  = $pf['Identity|MCU_Target'];  if ($mcut  -eq $null -or $mcut  -eq '') { $mcut  = $name.ToLower() }
    $tag   = $pf['Identity|Define_Tag'];  if ($tag   -eq $null -or $tag   -eq '') { $tag   = '__AVR__' }
    $dfc   = $pf['Identity|Default_F_CPU']; if ($dfc -eq $null -or $dfc -eq '') { $dfc = 8000000 }
    if ($mhz -eq '') { $fcpu = [string]$dfc } else { $fcpu = ([string]$mhz) + '000000' }

    Write-IniKey 'ActiveSession' 'ProcessorProfile' $chip
    Write-IniKey 'ActiveSession' 'MCU_Target'        $mcut
    Write-IniKey 'ActiveSession' 'Define_Tag'        $tag
    Write-IniKey 'ActiveSession' 'F_CPU'             ($fcpu + 'UL')

    # Makefile
    if (Test-Path $Mkfile) {
        $mk = [IO.File]::ReadAllText($Mkfile, [Text.Encoding]::UTF8)
        $mk = [regex]::Replace($mk, '(?i)^(MCU\s*=\s*).*$', '$1' + $mcut, 'Multiline')
        $mk = [regex]::Replace($mk, '(?i)^(F_CPU\s*=\s*).*$'  , '$1' + $fcpu + 'UL', 'Multiline')
        $enc = New-Object System.Text.UTF8Encoding($false); [IO.File]::WriteAllText($Mkfile, $mk, $enc)
    }
    # c_cpp_properties.json (правка строкой: JSON-парсера в старых PS нет)
    if (Test-Path $Props) {
        $pj = [IO.File]::ReadAllText($Props)
        $pj = [regex]::Replace($pj, "[A-Za-z_]+=\d+UL", 'F_CPU=' + $fcpu + 'UL')
        $pj = [regex]::Replace($pj, '__AVR_[A-Za-z0-9]+__', $tag)
        $e8 = New-Object System.Text.UTF8Encoding($false)
        [IO.File]::WriteAllText($Props, $pj, $e8)
    }
    Write-Host ('  ЧПу:  ' + $chip + '  (target ' + $mcut + ', tag ' + $tag + ')') -ForegroundColor Green
    Write-Host ('  F_CPU: ' + (([int]$fcpu) / 1000000) + ' MHz   -> echo.ctos + Makefile + c_cpp_properties.json') -ForegroundColor Green
    if ($tag -ne '__AVR_ATmega8__') {
        Write-Host '  Внимание: ядро эмулятора emu.exe пока работает с профилем памяти' -ForegroundColor Yellow
        Write-Host '  выбранного чипа через Procs-файл (FlashEnd/RamEnd/порты).' -ForegroundColor Yellow
    }
    return $true
}

function Proc-ListImpl() {
    Write-Host ('  Доступные профили (' + $Procs + '):') -ForegroundColor Cyan
    Write-Host ('  {0,-14}{1,-14}{2:>9}{3:>9}{4:>8}{5:>8}' -f 'Name','MCU_Target','Flash','SRAM','EEPROM','Ports')
    Get-ChildItem $Procs -Filter '*.proc' | ForEach-Object {
        $p = Read-ProFile $_.FullName
        $chip = $p['Identity|Name']; if ($chip -eq $null -or $chip -eq '') { $chip = [IO.Path]::GetFileNameWithoutExtension($_.Name) }
        $t = $p['Identity|MCU_Target']; if ($t -eq $null) { $t = '-' }
        $fe = Parse-Tc $p['Memory|FlashEnd']
        $re = Parse-Tc $p['Memory|RamEnd']
        $rs = Parse-Tc $p['Memory|SRAMStart']
        $ee = (Parse-Tc $p['Memory|EepromEnd']) + 1
        $ports = 0
        if (Test-Path $_.FullName) {
            $inPort = $false
            foreach ($l in (Get-Content $_.FullName)) {
                $t2 = ($l + '').Trim()
                if ($t2.StartsWith('[')) { $inPort = ($t2 -imatch '^\[ports\]'); }
                elseif ($inPort -and $t2 -ne '' -and -not $t2.StartsWith('#') -and $t2.Contains('=')) { $ports++ }
            }
        }
        $sram = ($re - $rs + 1) / 1024
        Write-Host ('  ' + $chip.PadRight(14) + $t.PadRight(14) + (('' + ($fe+1)/1024) + 'K').PadLeft(4) + (('' + $sram) + 'K').PadLeft(7) + (('' + $ee) + 'B').PadLeft(6) + ('' + $ports).PadLeft(7))
    }
}

# побитовые описания ключевых регистров (общие для семейства)
$BitInfo = @{
  'SREG'   = '7:I(гл.прерыв)|6:T(копиров.бита)|5:H(полуперен)|4:S(знак N^V)|3:V(переполн)|2:N(минус)|1:Z(ноль)|0:C(перенос)'
  'SPL'    = 'младший байт указателя стека SP'
  'SPH'    = 'старший байт указателя стека SP'
  'MCUCR'  = 'SE(сон)/SM2:SM0(режим сна)/ISC11,10(интерф.INT1)/ISC01,00(интерф.INT0); в m88+: PUD/BODS/BODSE,PUD,BODS,ISC11,ISC10,ISC01,ISC00'
  'MCUCSR' = '7:JTD 3:WDRF(сторож)|2:BORF(питание)|1:EXTRF(внешн.сброс)|0:PORF(вкл.питан.)'
  'MCUSR'  = '3:WDRF 2:BORF 1:EXTRF 0:PORF (причины последнего сброса)'
  'SFIOR'  = '4:ADHSM 2:PUD(запрет подтяжек) 0:PSR10(сброс преддел. T0/T1)'
  'SPMCSR' = '7:SPMIE 6:RWWSB 4:RWWSRE 3:BLBSET 2:PGWRT 1:PGERS 0:SPMEN (самопрошивка)'
  'SPMCR'  = '7:SPMIE 6:RWWSB 4:RWWSRE 3:BLBSET 2:PGWRT 1:PGERS 0:SPMEN (самопрошивка)'
  'TIMSK'  = '6:TOIE2 5:OCIE2 4:TICIE1 3:OCIE1A 2:OCIE1B 1:TOIE1 0:TOIE0'
  'TIFR'   = '6:TOV2 5:OCF2 4:ICF1 3:OCF1A 2:OCF1B 1:TOV1 0:TOV0 (флаги таймеров)'
  'GICR'   = '7:INT1 6:INT0 5:INT2 5b:IVCE/PUD... (разрешения внешн.прерываний)'
  'GIMSK'  = '7:INT1 6:INT0 (в tiny-чипах), PCIE-биты'
  'GIFR'   = '7:INTF1 6:INTF0 5:INTF2 (флаги внешних прерываний)'
  'WDTCR'  = '4:WDCE(изменить) 3:WDE(разрешить) 2..0:WDP (период сторожевого)'
  'WDTCSR' = '7:WDIF 6:WDIE 4:WDCE 3:WDE 2..0:WDP (m88+)'
  'ADCSRA' = '7:ADEN(вкл) 6:ADSC(старт) 5:ADATE 4:ADIF 3:ADIE 2..0:ADPS(преддел)'
  'ADMUX'  = '7:REFS1 6:REFS0 5:ADLAR 4..3:--  3..0:MUX (канал+опора)'
  'UCSRA'  = '7:RXC 6:TXC 5:UDRE 4:FE 3:DOR 2:UPE 1:U2X 0:MPCM'
  'UCSRB'  = '7:RXCIE 6:TXCIE 5:UDRIE 4:RXEN 3:TXEN 2:UCSZ2 1:RXB8 0:TXB8'
  'UCSRC'  = '7:URSEL 6..4:UMSEL/UPM 3:USBS 2..1:UCSZ 0:UCPOL'
  'SPCR'   = '7:SPIE 6:SPE 5:DORD 4:MSTR 3:CPOL 2:CPHA 1..0:SPR(скорость)'
  'SPSR'   = '7:SPIF 6:WCOL 0:SPI2X'
  'TWCR'   = '7:TWINT 6:TWEA 5:TWSTA 4:TWSTO 3:TWWC 2:TWEN 0:TWIE'
  'SMCR'   = '3:SE(сон разрешен) 2..0:SM (режимы сна) (m88+)'
  'TCCR0'  = 'биты CS00..2 (преддел), WGM, COM (8-бит таймер0)'
  'TCCR2'  = 'биты CS20..2 (преддел), WGM20/21, COM20/21 (8-бит таймер2)'
  'ASSR'   = 'асинхронный режим таймера 2 (мегаты старой серии)'
}

function Proc-InfoImpl($reg) {
    $prof = Get-Cs 'ActiveSession' 'ProcessorProfile' 'ATmega8A'
    $path = Join-Path $Procs ($prof + '.proc')
    if (-not (Test-Path $path)) {
        $alt = Get-ChildItem $Procs -Filter '*.proc' | Select-Object -First 1
        if ($alt -eq $null) { Write-Host 'Нет профилей в Procs' -ForegroundColor Red; return }
        $path = $alt.FullName
    }
    $pf = Read-ProFile $path
    if ($reg -eq '') {
        Write-Host ('=== Профиль ' + $prof + '  (' + $path + ')') -ForegroundColor Cyan
        $order = @('Identity','Memory','Ports','Interrupts','Cycles','GPIO rules')
        $groups = @{}
        foreach ($k in $pf.Keys) {
            $sk = $k.Split('|')
            if ($sk[0] -eq 'RegisterMap') { continue }
            if (-not $groups.ContainsKey($sk[0])) { $groups[$sk[0]] = @() }
            $groups[$sk[0]] = $groups[$sk[0]] + $k
        }
        foreach ($s in $order) {
            if ($s -notmatch '^GPIO' -and -not ($groups.ContainsKey($s) -or $groups.Contains($s))) { continue }
            Write-Host ('[' + $s + ']') -ForegroundColor Cyan
            $keys = $groups[$s]
            if ($s -eq 'Interrupts') { $keys = $keys | Sort-Object { [int]($_.Split('|')[1].Split('=')[0].Trim()) } }
            foreach ($k in $keys) { Write-Host ('  ' + $k.Split('|')[1]) }
            $groups.Remove($s)
        }
        foreach ($s in @($groups.Keys | Sort-Object)) {
            Write-Host ('[' + $s + ']') -ForegroundColor Cyan
            foreach ($k in $groups[$s]) { Write-Host ('  ' + $k.Split('|')[1]) }
        }
        $nreg = (($pf.Keys | Where-Object { $_.StartsWith('RegisterMap|') }).Count)
        Write-Host ('  Регистров в словаре: ' + $nreg) -ForegroundColor Gray
        Write-Host '  proc info <РЕГИСТР> - побитовая расшифровка (например: proc info SREG)' -ForegroundColor Gray
        return
    }
    $r = $reg.ToUpper()
    $kv = $pf['RegisterMap|' + $r]
    if ($kv -eq $null) { Write-Host ('  Регистр ' + $reg + ' не найден в ' + $prof) -ForegroundColor Red; return }
    $parts = $kv.Split('|')
    $kind = $parts[0]; $addr = $parts[1]
    $io = -1; $sram = -1
    if ($kind -ieq 'IO')  { $io = [Convert]::ToInt32($addr,16)+0; $sram = $io + 0x20 }
    elseif ($kind -ieq 'IO8') { $io = [Convert]::ToInt32($addr,16); $sram = $io + 0x20 }
    else { $sram = [Convert]::ToInt32($addr,16) }
    Write-Host ('=== ' + $r + '  (' + $prof + ')') -ForegroundColor Cyan
    if ($io -ge 0) {
        Write-Host ('  I/O адрес (IN/OUT):  0x' + ('{0:X2}' -f $io)) -ForegroundColor Green
    }
    Write-Host ('  SRAM адрес (LDS/STS): 0x' + ('{0:X4}' -f $sram)) -ForegroundColor Green
    if ($BitInfo.ContainsKey($r)) { Write-Host ('  Биты: ' + $BitInfo[$r]) -ForegroundColor Yellow }
    else { Write-Host '  Побитовое описание: см. datasheet чипа (не в общем словаре)' -ForegroundColor DarkGray }
}

# Вызов компилятора/утилит
$Toolchain = ''
function Get-Toolchain {
    if ($Toolchain -eq '') {
        $Toolchain = Get-Cs 'Paths' 'ToolchainBin' ''
        if ($Toolchain -eq '-' -or $Toolchain -eq $null -or $Toolchain -eq '') { $Toolchain = 'C:/Program Files (x86)/Atmel/AVR Tools/AVR Toolchain/bin' }
        $Toolchain = $Toolchain.TrimEnd('/')
    }
    return $Toolchain
}
function Find-Tool($exe) {
    $p = Join-Path (Get-Toolchain) $exe
    if (Test-Path $p) { return $p }
    Write-Host ('  ! Не найдено: ' + $p) -ForegroundColor Red
    return ''
}
function Build-MemoryReport() {
    $elf = Join-Path $Root 'main.elf'
    if (-not (Test-Path $elf)) { Write-Host '  main.elf не найден' -ForegroundColor Red; return }
    $sz = Find-Tool 'avr-size.exe'
    if ($sz -ne '') {
        $out = & $sz -C $elf 2>$null
        if ($out) { $out | ForEach-Object { Write-Host ('  ' + $_) } ; return }
    }
    # fallback: парсим objdump
    $od = Find-Tool 'avr-objdump.exe'
    if ($od -ne '') {
        $out = & $od -h $elf
        $txt = 0; $dat = 0; $bss = 0
        foreach ($l in $out) {
            if ($l -match '\.text\s+\S+\s+(\d+)') { $txt = [int]$Matches[1] }
            if ($l -match '\.data\s+\S+\s+(\d+)') { $dat = [int]$Matches[1] }
            if ($l -match '\.bss\s+\S+\s+(\d+)')  { $bss = [int]$Matches[1] }
        }
        Write-Host ('  Program: ' + ($txt+$dat) + ' bytes | Data: ' + $dat + ' | BSS: ' + $bss) -ForegroundColor Green
    }
}
function Build-ImplOptions($args0) {
    $opt = 'O0'
    if ($args0.Count -ge 2) {
        for ($i = 1; $i -lt $args0.Count; $i++) {
            if ($args0[$i] -imatch '^-?opt$' -and $args0.Count -gt $i+1) { $opt = ('' + $args0[$i+1]).ToUpper() }
            elseif ($args0[$i] -imatch '^O[0-3s]$') { $opt = ('' + $args0[$i]).ToUpper() }
        }
    }
    return $opt
}
function Build-Impl($a) {
    $src = Join-Path $Root 'main.c'
    if (-not (Test-Path $src)) { Write-Host '  main.c не найден в корне проекта' -ForegroundColor Red; return }
    $cc = Find-Tool 'avr-gcc.exe';     if ($cc -eq '') { return }
    $oc = Find-Tool 'avr-objcopy.exe'; if ($oc -eq '') { return }
    $opt = Build-ImplOptions $a
    $mcut = Get-Cs 'ActiveSession' 'MCU_Target' 'atmega8'
    $fcpu = Get-Cs 'ActiveSession' 'F_CPU' '8000000UL'
    Write-Host ('  [Build] ' + (Split-Path -Leaf $src) + '  mcu=' + $mcut + '  F_CPU=' + $fcpu + '  -' + $opt) -ForegroundColor Cyan
    & $cc ('-mmcu=' + $mcut) ('-DF_CPU=' + $fcpu) ('-' + $opt) -Wall -g $src -o (Join-Path $Root 'main.elf')
    if ($LASTEXITCODE -ne 0) { Write-Host '  Сборка ОШИБКА (см. выше)' -ForegroundColor Red; return }
    & $oc -O ihex -R .eeprom (Join-Path $Root 'main.elf') (Join-Path $Root 'main.hex')
    Write-Host '  Сборка OK: main.elf + main.hex (с отладочной информацией -g для построчной эмуляции)' -ForegroundColor Green
    Build-MemoryReport
}
function Hex-Impl() {
    $oc = Find-Tool 'avr-objcopy.exe'; if ($oc -eq '') { return }
    & $oc -O ihex -R .eeprom (Join-Path $Root 'main.elf') (Join-Path $Root 'main.hex')
    Write-Host '  main.hex обновлен' -ForegroundColor Green
}
function Clean-Impl() {
    foreach ($ext in @('*.elf','*.hex','*.o','*.map','*.lss')) {
        Get-ChildItem $Root -Filter $ext | ForEach-Object { Remove-Item $_.FullName -Force }
    }
    Write-Host '  Очистка: elf/hex/o/map/lss удалены' -ForegroundColor Green
}
function Size-Impl() { Build-MemoryReport }
function Disasm-Impl($a) {
    $od = Find-Tool 'avr-objdump.exe'; if ($od -eq '') { return }
    $elf = Join-Path $Root 'main.elf'
    if (-not (Test-Path $elf)) { Write-Host '  main.elf не найден. Сначала: build' -ForegroundColor Red; return }
    $out = @( & $od -d -S $elf )
    if ($a.Count -ge 2) {
        # Старый binutils не понимает --disassemble=<sym>: фильтруем по метке <sym>:
        $sym = $a[1]
        $needle = '<' + $sym + '>:'
        $start = -1
        foreach ($l in $out) {
            $start++
            if ((('' + $l).Trim()).EndsWith($needle)) { break }
            if ($start -ge $out.Count - 1) { $start = -1 }
        }
        if ($start -lt 0) { Write-Host ('  Символ не найден: ' + $sym) -ForegroundColor Red; return }
        $j = $start + 1
        while ($j -lt $out.Count -and -not ((('' + $out[$j]).Trim()).EndsWith('>:'))) { $j++ }
        if ($j -ge $out.Count) { $j = $out.Count - 1 }
        $out[$start..$j] | ForEach-Object { Write-Host $_ }
        return
    }
    $out | ForEach-Object { Write-Host $_ }
}
function Flash-Impl($a) {
    $ad = ''
    $cand = Join-Path (Get-Toolchain) 'avrdude.exe'
    if (Test-Path $cand) { $ad = $cand } else {
        Get-ChildItem (Join-Path (Get-Toolchain) '..') -Recurse -Filter 'avrdude.exe' -ErrorAction SilentlyContinue | Select-Object -First 1 | ForEach-Object { $ad = $_.FullName }
    }
    if ($ad -eq '') { Write-Host '  avrdude.exe не найден в тулчейне. Прошивка вручную (AVR Prog/lede)' -ForegroundColor Red; return }
    $mcut = Get-Cs 'ActiveSession' 'MCU_Target' 'atmega8'
    $port = 'usbasp'; if ($a.Count -ge 2) { $port = $a[1] }
    Write-Host ('  [Flash] ' + $mcut + ' <- main.hex  (programmer: ' + $port + ')') -ForegroundColor Cyan
    & $ad -c $port -p $mcut -U ('flash:w:' + (Join-Path $Root 'main.hex') + ':i')
}
function Emu-Impl($a) {
    $emuPs = Join-Path $Root 'build\emu.ps1'
    if (-not (Test-Path $emuPs)) { Write-Host '  build\emu.ps1 не найден' -ForegroundColor Red; return }
    $elf = Join-Path $Root 'main.elf'
    if (-not (Test-Path $elf)) { Write-Host '  main.elf не найден. Сначала: build' -ForegroundColor Red; return }
    $exe = Join-Path $Root 'build\emu.exe'
    if (-not (Test-Path $exe)) { Write-Host '  build\emu.exe not found. Build it first:' -ForegroundColor Red; Write-Host ('  g++ -O2 -std=c++17 -o "' + $exe + '" "' + (Join-Path $Root 'build\emu.cpp') + '"') -ForegroundColor Yellow; return }
    $oldDir = Get-Location
    Set-Location $Root
    try {
        if ($a.Count -ge 2) { & $exe -e $elf -f $a[1] }
        else { & $exe -e $elf }
    }
    finally { Set-Location $oldDir }
}
function Status-Impl() {
    $ini = Read-Ini
    Write-Host '=== Статус сессии AVR CLI Studio' -ForegroundColor Cyan
    foreach ($k in ($ini.Keys | Sort-Object)) { Write-Host ('  ' + $k.PadRight(34) + ' = ' + $ini[$k]) }
    foreach ($f in @('main.c','main.elf','main.hex','build\emu.exe','build\emu.cpp')) {
        $ok = Test-Path (Join-Path $Root $f)
        Write-Host ('  ' + $f.PadRight(30) + $(if ($ok) { '[OK]' } else { '[нет]' }) ) -ForegroundColor $(if ($ok) { 'Green' } else { 'DarkGray' })
    }
}
function Help-Impl() {
    Write-Host @'
Команды (алиас):
  proc list            | pl       список профилей Procs
  proc select <чип> [МГц] | ps  выбрать чип (echo.ctos+Makefile+JSON)
  proc info [<РЕГ>]    | pi       справка профиля/регистра (биты)
  build [-opt O0|O1|O2|Os] | b   компиляция main.c -> main.elf/.hex
  size                 | sz       память Flash/SRAM (окно Build)
  clean                | cl       удалить elf/hex/o/map/lss
  hex                  | hx       пересобрать только main.hex
  disasm [<функция>]   | da       дизассемблер (objdump -dS)
  flash [<программатор>]          прошивка через avrdude (usbasp)
  emu [<скрипт>]       | em       запуск эмулятора (main.elf)
  status               | st       состояние сессии (echo.ctos)
  help                 | h        эта справка
  exit                 | q        выход
'@
}

function Invoke-Line($line) {
    $line = ('' + $line).Trim()
    if ($line -eq '') { return $true }
    $parts = $line -split '\s+'
    $c = $parts[0].ToLower()
    # раскладка "xxx yyy" из README (proc list / proc select / proc info)
    if ($c -ieq 'proc' -or $c -ieq 'pl' -or $c -ieq 'ps' -or $c -ieq 'pi') {
        $sub = ''
        if ($parts.Count -ge 2) { $sub = $parts[1].ToLower() }
        if ($c -ieq 'pl' -or ($c -ieq 'proc' -and $sub -eq 'list'))    { Proc-ListImpl; return $true }
        if ($c -ieq 'pi' -or ($c -ieq 'proc' -and $sub -eq 'info')) {
            $reg = ''; $base = 1; if ($c -ieq 'proc') { $base = 2 }
            if ($parts.Count -ge $base + 1) { $reg = $parts[$base] }
            Proc-InfoImpl $reg; return $true
        }
        if ($c -ieq 'ps' -or ($c -ieq 'proc' -and $sub -eq 'select')) {
            $name = ''; $mhz = ''
            if ($c -ieq 'ps') { if ($parts.Count -ge 2) { $name = $parts[1] }; if ($parts.Count -ge 3) { $mhz = $parts[2] } }
            else             { if ($parts.Count -ge 3) { $name = $parts[2] }; if ($parts.Count -ge 4) { $mhz = $parts[3] } }
            if ($name -eq '') { Write-Host '  Формат: proc select <имя_чипа> [МГц]' -ForegroundColor Red; return $true }
            Select-ProcImpl $name $mhz | Out-Null; return $true
        }
        if ($c -ieq 'proc') { Write-Host '  proc list | proc select <чип> [МГц] | proc info [<РЕГ>]' -ForegroundColor Gray; return $true }
    }
    switch ($c) {
        'b'      { Build-Impl $parts; }
        'build'  { Build-Impl $parts; }
        'cl'     { Clean-Impl }
        'clean'  { Clean-Impl }
        'sz'     { Size-Impl }
        'size'   { Size-Impl }
        'hx'     { Hex-Impl }
        'hex'    { Hex-Impl }
        'da'     { Disasm-Impl $parts }
        'disasm' { Disasm-Impl $parts }
        'flash'  { Flash-Impl $parts }
        'em'     { Emu-Impl $parts }
        'emu'    { Emu-Impl $parts }
        'es'     { Emu-Impl @() }
        'start'  { Emu-Impl @() }
        'st'     { Status-Impl }
        'status' { Status-Impl }
        'h'      { Help-Impl }
        'help'   { Help-Impl }
        '?'      { Help-Impl }
        'q'      { return $false }
        'exit'   { return $false }
        'quit'   { return $false }
        default  { Write-Host ('  Неизвестная команда: ' + $c + ' (help - справка)') -ForegroundColor Red }
    }
    return $true
}

# ---------------------------- точка входа ----------------------------
if ($args.Count -ge 1) {
    Invoke-Line ($args -join ' ') | Out-Null
    exit $LASTEXITCODE
}
Write-Host ('AVR CLI Studio shell  |  root: ' + $Root) -ForegroundColor Cyan
Write-Host 'help - справка.  Пример: proc select atmega8 8 / build -opt O0 / emu' -ForegroundColor Gray
while ($true) {
    Write-Host 'AVR-CLI> ' -NoNewline
    $line = Read-Host
    if (-not (Invoke-Line $line)) { break }
}
