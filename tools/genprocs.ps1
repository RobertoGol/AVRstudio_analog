# =====================================================================
#  AVR CLI Studio - Генератор профилей процессоров  |  genprocs.ps1
#  Создаёт Procs/<Chip>.proc автоматически из заголовков AVR Toolchain
#  (avr-gcc 4.x, оба формата: 2005 г. iom8.h и 2011+ iom16a.h/iox*.h).
#  Контент канонический и полный:
#    [Identity]       - имя из Define_Tag в io.h (то, что видит avr-gcc)
#    [Signature]      - SIGNATURE_0/1/2 (id чипа для SPM BLBSET / avrdude)
#    [Memory]         - FLASHEND / RAMEND / E2END / SPM_PAGESIZE
#                       (RAMSTART+RAMSIZE-1 и PROGMEM_SIZE тоже вычисляются)
#    [Ports]          - тройки PINx/DDRx/PORTx в I/O пространстве (IN/OUT)
#    [Interrupts]     - все вектора _VECTOR(N) в порядке железных индексов
#    [RegisterMap]    - ВСЕ регистры _SFR_IO8 + _SFR_MEM8 (расшир. I/O -> MEM8)
#  Инструмент сопровождения: достаточно PowerShell 2.0+. Ручные профили
#  (ATmega8A/328P/ATtiny13/...) НЕ перезаписываются без явной опции.
#
#  Запуск:   powershell -File tools\genprocs.ps1             (добавить недостающие)
#            powershell -File tools\genprocs.ps1 -Overwrite  (перегенерить всё)
# =====================================================================
param([switch]$Overwrite)

$Root = Split-Path -Parent $PSScriptRoot
$Procs = Join-Path $Root 'Procs'

# --- тулчейн: echo.ctos -> классический путь (как в avr.ps1) ---
$Inc = ''
$ctos = Join-Path $Root 'echo.ctos'
if (Test-Path $ctos) {
    foreach ($ln in [IO.File]::ReadAllLines($ctos, [Text.Encoding]::UTF8)) {
        $t = ('' + $ln).Trim()
        if ($t -match '^ToolchainInc\s*=\s*(.+)$') { $Inc = $Matches[1].Trim(); break }
    }
}
if ($Inc -ne '' -and -not (Test-Path (Join-Path $Inc 'io.h')) -and (Test-Path (Join-Path $Inc 'avr\io.h'))) {
    $Inc = Join-Path $Inc 'avr'
}
if ($Inc -eq '' -or -not (Test-Path (Join-Path $Inc 'io.h'))) {
    $Inc = 'C:\Program Files (x86)\Atmel\AVR Tools\AVR Toolchain\avr\include\avr'
}
if (-not (Test-Path (Join-Path $Inc 'io.h'))) {
    Write-Host "  ! Не найден заголовочный каталог тулчейна: $Inc" -ForegroundColor Red
    Write-Host '    Проверь echo.ctos [Paths] ToolchainInc = ...' -ForegroundColor Yellow
    return
}
Write-Host "  Заголовки: $Inc" -ForegroundColor Cyan

# --- шаблоны-хвосты [Cycles] + [GPIO rules] из эталонного профиля ---
$tmplCycles = ''
$tmplPath = Join-Path $Procs 'ATmega8A.proc'
if (Test-Path $tmplPath) {
    $tmpl = [IO.File]::ReadAllText($tmplPath, [Text.Encoding]::UTF8)
    $i = $tmpl.IndexOf('[Cycles]')
    if ($i -ge 0) { $tmplCycles = $tmpl.Substring($i) }
}

# ---------------------------------------------------------------------
# Парс io.h: тег Define -> заголовок (io.h: #elif defined (__AVR_..__))
# ---------------------------------------------------------------------
$map = New-Object System.Collections.ArrayList        # @{Tag;Header}
$held = New-Object System.Collections.ArrayList
foreach ($ln in [IO.File]::ReadAllLines((Join-Path $Inc 'io.h'), [Text.Encoding]::UTF8)) {
    $l = ('' + $ln).Trim()
    if ($l -match '^#\s*(if|elif)\b') {
        if ($l -match '__AVR_') { $held.Clear() }
        $ms = [regex]::Matches($l, '__AVR_([A-Za-z0-9]+)(__)')
        foreach ($m in $ms) { [void]$held.Add($m.Groups[1].Value) }
        continue
    }
    if ($l -match '^#\s*include\s+<avr/(io[A-Za-z0-9_]+)\.h>') {
        $hdr = $Matches[1]
        foreach ($t in $held) { [void]$map.Add(@{ Tag = $t; Header = $hdr }) }
        $held.Clear()
    }
}
Write-Host ('  Карта io.h: ' + $map.Count + ' тегов Define') -ForegroundColor Gray

# ---------------------------------------------------------------------
# Парс одного заголовка чипа: регистры / векторы / память / сигнатура
# ---------------------------------------------------------------------
function Parse-Header($path) {
    $r = @{ regs = @(); vect = @(); flash = -1; ram = -1; e2 = -1; ioend = -1; spm = -1;
            ramstart = -1; ramsize = -1; pgmsize = -1; sig = New-Object 'int[]' 3;
            isramStart = -1; isramSize = -1; flashId = ''; ramId = ''; ramstartId = '' }
    $rxReg = '^#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\s+_SFR_(IO8|MEM8)\(\s*(0[xX][0-9A-Fa-f]+|[0-9]+)\s*\)'
    $rxVec = '^#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\s+_VECTOR\(\s*([0-9]+)\s*\)'
    # значения бывают: 0x1FFF | (0x1FFF) | (RAMSTART + RAMSIZE - 1) -> числа отдельно
    $rxMem = '^#\s*define\s+(FLASHEND|RAMEND|RAMSTART|RAMSIZE|E2END|IOEND|SPM_PAGESIZE|PROGMEM_SIZE|INTERNAL_SRAM_START|INTERNAL_SRAM_SIZE)\s+[\(]?\s*(0[xX][0-9A-Fa-f]+|[0-9]+)\s*[\)]?'
    # xmega: значения - идентификаторы (RAMEND INTERNAL_SRAM_END и т.п.)
    $rxId  = '^#\s*define\s+(FLASHEND|RAMEND|RAMSTART)\s+([A-Za-z_][A-Za-z0-9_]*)\s*$'
    $rxSig = '^#\s*define\s+SIGNATURE_([012])\s+[\(]?\s*(0[xX][0-9A-Fa-f]+|[0-9]+)\s*[\)]?'
    foreach ($ln in [IO.File]::ReadAllLines($path, [Text.Encoding]::UTF8)) {
        $l = ('' + $ln).Trim()
        if ($l -eq '' -or $l.StartsWith('/*')) { continue }

        # ВАЖНО: $Matches перезаписывается каждым -match, копируем группы сразу
        if ($l -match $rxId) {
            $key = $Matches[1]; $idv = $Matches[2]
            if ($key -ieq 'FLASHEND') { $r.flashId = $idv }
            elseif ($key -ieq 'RAMEND') { $r.ramId = $idv }
            else { $r.ramstartId = $idv }
            continue
        }
        if ($l -match $rxMem) {
            $key = $Matches[1]; $val = $Matches[2]
            $v = 0
            if ($val.StartsWith('0x') -or $val.StartsWith('0X')) { $v = [Convert]::ToInt32($val.Substring(2),16) }
            else { $v = [int]$val }
            switch ($key) {
                'FLASHEND'     { if ($r.flash -lt 0) { $r.flash = $v } }
                'RAMEND'       { if ($r.ram   -lt 0) { $r.ram   = $v } }
                'RAMSTART'     { if ($r.ramstart -lt 0) { $r.ramstart = $v } }
                'RAMSIZE'      { if ($r.ramsize  -lt 0) { $r.ramsize  = $v } }
                'PROGMEM_SIZE' { if ($r.pgmsize  -lt 0) { $r.pgmsize  = $v } }
                'E2END'        { if ($r.e2    -lt 0) { $r.e2    = $v } }
                'IOEND'        { if ($r.ioend -lt 0) { $r.ioend = $v } }
                'SPM_PAGESIZE' { if ($r.spm   -lt 0) { $r.spm   = $v } }
                'INTERNAL_SRAM_START' { if ($r.isramStart -lt 0) { $r.isramStart = $v } }
                'INTERNAL_SRAM_SIZE'  { if ($r.isramSize  -lt 0) { $r.isramSize  = $v } }
            }
            continue
        }
        if ($l -match $rxSig) {
            $si = [int]$Matches[1]; $val = $Matches[2]
            $v = 0
            if ($val.StartsWith('0x') -or $val.StartsWith('0X')) { $v = [Convert]::ToInt32($val.Substring(2),16) }
            else { $v = [int]$val }
            $r.sig[$si] = $v
            continue
        }
        if ($l -match $rxReg) {
            $name = $Matches[1]; $kind = $Matches[2]; $raw = $Matches[3]
            $addr = 0
            if ($raw.StartsWith('0x') -or $raw.StartsWith('0X')) { $addr = [Convert]::ToInt32($raw.Substring(2),16) }
            else { $addr = [int]$raw }
            if ($kind -ieq 'IO8') {
                $r.regs += ,@{ N = $name; K = 'IO8'; A = $addr }
            } else {
                # _SFR_MEM8(x) = SRAM-адрес; в I/O пространстве это x-0x20
                if ($addr -ge 0x20 -and $addr -le 0x5F) { $r.regs += ,@{ N = $name; K = 'IO8'; A = $addr - 0x20 } }
                else { $r.regs += ,@{ N = $name; K = 'MEM8'; A = $addr } }
            }
            continue
        }
        if ($l -match $rxVec) {
            $vi = [int]$Matches[2]
            if ($vi -ne 0) { $r.vect += ,@{ I = $vi; N = $Matches[1] } }
            continue
        }
    }
    # вычисляемые: RAMSTART+RAMSIZE-1, PROGMEM_SIZE-1, идентификаторы xmega
    if ($r.ram -lt 0 -and $r.ramstart -ge 0 -and $r.ramsize -gt 0) { $r.ram = $r.ramstart + $r.ramsize - 1 }
    if ($r.flash -lt 0 -and $r.pgmsize -gt 0) { $r.flash = $r.pgmsize - 1 }
    if ($r.ramstart -lt 0 -and $r.ramstartId -ieq 'INTERNAL_SRAM_START' -and $r.isramStart -ge 0) { $r.ramstart = $r.isramStart }
    if ($r.ram -lt 0 -and $r.ramId -ieq 'INTERNAL_SRAM_END' -and $r.isramStart -ge 0 -and $r.isramSize -gt 0) { $r.ram = $r.isramStart + $r.isramSize - 1 }
    if ($r.flash -lt 0 -and $r.flashId -ieq 'PROGMEM_END' -and $r.pgmsize -gt 0) { $r.flash = $r.pgmsize - 1 }
    return $r
}

# Второй проход: заголовки, о которых io.h 4.5.1 молчит (новые алиасы A8/PA/U-
# вариантов из списка AVR Studio), т.е. тег берём из гварда #ifndef _AVR_XXX_H_
$knownHeaders = @{}
foreach ($e in $map) { if (-not $knownHeaders.ContainsKey($e.Header)) { $knownHeaders[$e.Header] = 1 } }
$added2 = 0
# общие "семейные" заголовки (регистры-супермножество без собственной геометрии)
$FamilyHeads = @('iocanxx','iomx8','iomxx0_1','iomxx4','iomxxhva','iotnx4','iotnx5','iotnx61','iousbxx2','iousbxx6_7')
foreach ($f in @(Get-ChildItem $Inc -Filter 'io*.h')) {
    $hdr = [IO.Path]::GetFileNameWithoutExtension($f.Name)
    if ($hdr -ieq 'io' -or $hdr -ieq 'iomacros' -or $hdr -ieq 'io_none') { continue }
    if ($knownHeaders.ContainsKey($hdr)) { continue }
    if ($FamilyHeads -icontains $hdr) { continue }
    $g = ''
    foreach ($ln in [IO.File]::ReadAllLines($f.FullName, [Text.Encoding]::UTF8)) {
        if ($ln -match '#ifndef\s+_AVR_([A-Za-z0-9]+)_H_') { $g = $Matches[1]; break }
    }
    if ($g -ne '' -and $g -imatch '^[A-Za-z0-9]+$') {
        [void]$map.Add(@{ Tag = $g; Header = $hdr })
        $added2++
    }
}
if ($added2 -gt 0) { Write-Host ('  Доп. теги из каталога заголовков: ' + $added2) -ForegroundColor Gray }

$written = 0; $skipped = 0; $failed = 0

# Заглушки в тулчейне 4.5.1 (файл по 39 байт, определения отсутствуют):
# геометрия берётся из даташитов, чтобы в Procs лежал ВЕСЬ официальный список чипов
$Stubs = @{
  'AT90PWM161'    = @{ Flash = 0x1FFF;  Ram = 0x045F;  E2 = 0x01FF }   # 8K/1K/512B
  'ATxmega128B1'  = @{ Flash = 0x1FFFF; Ram = 0x3FFF;  E2 = 0x07FF }   # 128K/8K/2K
  'ATxmega256A3BU'= @{ Flash = 0x3FFFF; Ram = 0x5FFF;  E2 = 0x0FFF }   # 256K/16K/4K
}

foreach ($e in $map) {
    $tag = $e.Tag; $hdr = $e.Header
    $out = Join-Path $Procs ($tag + '.proc')
    if (Test-Path $out) {
        # перегенерируем только AUTOGEN; ручные профили защищены
        $isAuto = $false
        foreach ($oln in ([IO.File]::ReadAllLines($out, [Text.Encoding]::UTF8) | Select-Object -First 3)) {
            if ($oln -match 'AUTOGEN') { $isAuto = $true }
        }
        if (-not $isAuto -and -not $Overwrite) { $skipped++; continue }
    }

    $path = Join-Path $Inc ($hdr + '.h')
    if (-not (Test-Path $path)) {
        $failed++
        Write-Host ('  ! Пропуск ' + $tag + ': нет ' + $hdr + '.h') -ForegroundColor DarkYellow
        continue
    }
    $d = Parse-Header $path
    # обёрточные заголовки (iom48.h, iom1280.h, iotn25.h, ...) тянут регистры и
    # векторы из семейных (iomx8, iomxx0_1, iotnx5, iocanxx, iousbxx*): идём за #include
    if ($d.regs.Count -eq 0 -or $d.vect.Count -eq 0) {
        foreach ($ln in [IO.File]::ReadAllLines($path, [Text.Encoding]::UTF8)) {
            if ($ln -match '^#include\s+<avr/(io[A-Za-z0-9_]+)\.h>') {
                $ih = $Matches[1]
                $ipath = Join-Path $Inc ($ih + '.h')
                if (Test-Path $ipath) {
                    $d2 = Parse-Header $ipath
                    if ($d.regs.Count -eq 0) { $d.regs = $d2.regs }
                    if ($d.vect.Count  -eq 0) { $d.vect  = $d2.vect }
                    if ($d.ioend -lt 0 -and $d2.ioend -ge 0) { $d.ioend = $d2.ioend }
                    if ($d.spm -lt 0 -and $d2.spm -ge 0) { $d.spm = $d2.spm }
                    if ($d.sig[0] -le 0 -and $d2.sig[0] -gt 0) { $d.sig = $d2.sig; $d.e2 = $d2.e2 }
                }
            }
        }
    }
    if ($d.flash -lt 0 -and $Stubs.ContainsKey($tag)) {
        $d.flash = $Stubs[$tag].Flash; $d.ram = $Stubs[$tag].Ram; $d.e2 = $Stubs[$tag].E2
        Write-Host ('  ~ ' + $tag + ': геометрия из таблицы-исключения (заглушка ' + $hdr + '.h)') -ForegroundColor DarkCyan
    }
    if ($d.flash -lt 0 -or $d.ram -lt 0) {
        $failed++
        Write-Host ('  ! Пропуск ' + $tag + ': нет FLASHEND/RAMEND в ' + $hdr + '.h') -ForegroundColor DarkYellow
        continue
    }

    # --- порты: тройки PINx / DDRx / PORTx (I/O пространство) ---
    $pinBy = @{}; $ddrBy = @{}; $portBy = @{}
    foreach ($g in $d.regs) {
        if ($g.K -ne 'IO8' -or $g.A -gt 0x3F) { continue }
        if ($g.N -match '^PIN([A-Z])$'  -and -not $pinBy.ContainsKey($Matches[1]))  { $pinBy[$Matches[1]]  = $g.A }
        if ($g.N -match '^DDR([A-Z])$'  -and -not $ddrBy.ContainsKey($Matches[1]))  { $ddrBy[$Matches[1]]  = $g.A }
        if ($g.N -match '^PORT([A-Z])$' -and -not $portBy.ContainsKey($Matches[1])) { $portBy[$Matches[1]] = $g.A }
    }
    $portLines = ''
    $letters = @(); foreach ($k in $pinBy.Keys) { $letters += $k }
    $letters = @($letters | Sort-Object)
    foreach ($L in $letters) {
        if ($ddrBy.ContainsKey($L) -and $portBy.ContainsKey($L)) {
            $portLines += ('  {0} = 0x{1:x2} 0x{2:x2} 0x{3:x2}' -f $L, $pinBy[$L], $ddrBy[$L], $portBy[$L]) + "`r`n"
        }
    }

    # --- RegisterMap: все регистры, по возрастанию адреса ---
    $rm = ''
    $sorted = @($d.regs | Sort-Object { $_.A }, { $_.N })
    $seen = @{}
    foreach ($g in $sorted) {
        $key = $g.N.ToUpper()
        if ($seen.ContainsKey($key)) { continue }
        $seen[$key] = 1
        $rm += ('{0}|{1}|0x{2:X2}' -f $key, $g.K, $g.A) + "`r`n"
    }

    # --- Interrupts: уникальный индекс, приоритет современного *_vect ---
    $vd = @{}
    foreach ($v in $d.vect) {
        if ($v.N -imatch '_vect$' -and -not $vd.ContainsKey($v.I)) { $vd[$v.I] = $v.N }
    }
    foreach ($v in $d.vect) {
        if (-not $vd.ContainsKey($v.I)) { $vd[$v.I] = $v.N }
    }
    $vi = ''
    foreach ($k in @($vd.Keys | Sort-Object)) {
        $vi += ('{0} = {1}' -f $k, $vd[$k]) + "`r`n"
    }

    # --- Signature + Memory ---
    $sgl = ''
    if ($d.sig[0] -gt 0 -and $d.sig[1] -gt 0 -and $d.sig[2] -gt 0) {
        $sgl = ('Signature = 0x{0:X2} 0x{1:X2} 0x{2:X2}   # device id (SPM BLBSET / avrdude)' -f $d.sig[0], $d.sig[1], $d.sig[2]) + "`r`n"
    }
    $kb = [Math]::Ceiling(($d.flash + 1) / 1024)
    $mem = ''
    $mem += ('  FlashEnd = 0x{0:X4}            # последний байт Flash ({1} КБ)' -f $d.flash, $kb) + "`r`n"
    $mem += '  SRAMStart = 0x0060            # начало ОЗУ данных' + "`r`n"
    $mem += ('  RamEnd = 0x{0:X4}            # вершина стека SP' -f $d.ram) + "`r`n"
    if ($d.e2 -ge 0)  { $mem += ('  EepromEnd = 0x{0:X4}' -f $d.e2) + "`r`n" }
    if ($d.spm -gt 0) { $mem += ('  SpmPagesize = {0}' -f $d.spm) + "`r`n" }

    $NL = "`r`n"
    $txt = '# AVR CLI Studio processor profile - AUTOGEN from ' + $hdr + '.h (tools/genprocs.ps1)' + $NL
    $txt += '# Источник: заголовок тулчейна avr-gcc. Ручные правки потеряются при -Overwrite.' + $NL + $NL
    $txt += '[Identity]' + $NL
    $txt += 'Name = ' + $tag + $NL
    $txt += 'MCU_Target = ' + $tag.ToLower() + $NL
    $txt += 'Define_Tag = __AVR_' + $tag + '__' + $NL
    $txt += 'Default_F_CPU = 1000000' + $NL + $NL
    if ($sgl -ne '') { $txt += '[Signature]' + $NL + $sgl + $NL }
    $txt += '[Memory]' + $NL + $mem + $NL
    if ($portLines -ne '') { $txt += '[Ports]' + $NL + $portLines + $NL }
    else { $txt += '[Ports]' + $NL + '# порты вне I/O-пространства (расширенные или отсутствуют)' + $NL + $NL }
    if ($vi -ne '') { $txt += '[Interrupts]' + $NL + $vi + $NL }
    if ($rm -ne '') { $txt += '[RegisterMap]' + $NL + $rm + $NL }
    $txt += $tmplCycles

    $enc = New-Object System.Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($out, $txt, $enc)
    $written++
}

# Алиасы AVR Studio: чипы-ревизии, у которых в тулчейне 4.5.1 нет заголовка.
# Геометрия/периферия идентична базовому кристаллу; MCU_Target оставляем от
# базового профиля, чтобы сборка -mmcu работала в ЭТОМ тулчейне.
$Aliases = @{
  'AT90SCR100H'   = 'AT90SCR100'    # ревизия H
  'ATmega1284'    = 'ATmega1284P'   # P = отладочный вариант того же кристалла
  'ATmega128A'    = 'ATmega128'
  'ATmega164PA'   = 'ATmega164P'
  'ATmega165PA'   = 'ATmega165P'
  'ATmega168PA'   = 'ATmega168P'
  'ATmega32A'     = 'ATmega32'
  'ATmega64A'     = 'ATmega64'
  'ATxmega128A3U' = 'ATxmega128A3'
  'ATxmega16A4U'  = 'ATxmega16A4'
  'ATxmega192A3U' = 'ATxmega192A3'
  'ATxmega256A3U' = 'ATxmega256A3'
  'ATxmega32A4U'  = 'ATxmega32A4'
  'ATxmega64A3U'  = 'ATxmega64A3'
}
foreach ($aName in $Aliases.Keys) {
    $aliasEx = Join-Path $Procs ($aName + '.proc')
    $srcA = Join-Path $Procs ($Aliases[$aName] + '.proc')
    if (-not (Test-Path $srcA)) {
        Write-Host ('  ! Алиас ' + $aName + ': нет источника ' + $Aliases[$aName]) -ForegroundColor DarkYellow
        continue
    }
    $c2 = [IO.File]::ReadAllText($srcA, [Text.Encoding]::UTF8)
    $base = $Aliases[$aName]
    $c2 = $c2 -replace ('Name = ' + $base + '\b'), ('Name = ' + $aName)
    if ($base -ieq 'AT90SCR100') {
        $c2 = $c2 -replace 'MCU_Target = at90scr100', 'MCU_Target = at90scr100'
    }
    $baseTag = $base
    $c2 = $c2 -replace ('Define_Tag = __AVR_' + $baseTag + '__'), ('Define_Tag = __AVR_' + $aName + '__')
    $c2 = $c2 -replace ('from ' + $baseTag + '\.proc'), ('from ' + $baseTag + '.proc (device alias ' + $aName + ')')
    # файл-источник не трогаем: помечаем в комментарии
    $c2 = ('# device alias of ' + $base + ' (нет отдельного заголовка в тулчейне 4.5.1)' + "`r`n") + $c2
    [IO.File]::WriteAllText($aliasEx, $c2, (New-Object System.Text.UTF8Encoding $false))
    Write-Host ('  ~ ' + $aName + ': алиасный профиль на базе ' + $base) -ForegroundColor DarkCyan
    $written++
}

Write-Host ''
Write-Host ('  Итог: сгенерировано ' + $written + ', пропущено (уже есть) ' + $skipped + ', ошибок ' + $failed) -ForegroundColor Green
Write-Host ('  Всего профилей в Procs: ' + (Get-ChildItem $Procs -Filter '*.proc').Count) -ForegroundColor Cyan