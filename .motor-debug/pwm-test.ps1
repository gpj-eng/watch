$ErrorActionPreference = 'Stop'
$port = [IO.Ports.SerialPort]::new('COM9', 1000000, 'None', 8, 'One')
$port.DtrEnable = $false
$port.RtsEnable = $false
$log = [Text.StringBuilder]::new()
function Send-Cmd([string]$command, [int]$delay = 100) {
    [void]$log.AppendLine('> ' + $command)
    $port.Write($command + "`r`n")
    Start-Sleep -Milliseconds $delay
    $reply = $port.ReadExisting()
    [void]$log.Append($reply)
    if ($reply -match '(error|Unknown|syntax|undefined|not found)') {
        throw ('Device rejected command: ' + $command + ': ' + $reply)
    }
    return $reply
}
try {
    $port.Open()
    $null = Send-Cmd 'exit'
    $idle = Send-Cmd '*(unsigned int*)0x50090000;'
    if ($idle -notmatch '0x00000004') { throw ('GPTIM1 not in expected idle state: ' + $idle) }
    $route = Send-Cmd '*(unsigned int*)0x5000b064;'
    if ($route -notmatch '0x3f3f3f3f') { throw ('GPTIM1 routing is already in use: ' + $route) }
    $rail = Send-Cmd '*(unsigned int*)0x500ca05c;'
    if ($rail -notmatch '0x000a2b19') { throw ('Unexpected PERI_LDO baseline: ' + $rail) }
    # Same operation as HAL_PMU_ConfigPeriLdo(PMU_PERI_LDO3_3V3,true,true):
    # clear PD bit21, set EN bit16, preserve trim settings and other rails.
    $null = Send-Cmd '*(unsigned int*)0x500ca05c=0x000b2b19;' 120
    $enabled = Send-Cmd '*(unsigned int*)0x500ca05c;'
    if ($enabled -notmatch '0x000b2b19') { throw ('Motor LDO enable failed: ' + $enabled) }
    # Official SiFli v2.4.0 register.h, gpt.h and bf0_hal_tim.h.
    # PCLK1 was read as 120 MHz. 120MHz / (119+1) / (999+1) = 1kHz.
    $null = Send-Cmd '*(unsigned int*)0x50090000=4;'
    $null = Send-Cmd '*(unsigned int*)0x50090020=0;'
    $null = Send-Cmd '*(unsigned int*)0x50090008=0;'
    $null = Send-Cmd '*(unsigned int*)0x5009000c=0;'
    $null = Send-Cmd '*(unsigned int*)0x50090028=119;'
    $null = Send-Cmd '*(unsigned int*)0x5009002c=999;'
    $null = Send-Cmd '*(unsigned int*)0x50090038=500;'
    $null = Send-Cmd '*(unsigned int*)0x50090018=0x68000000;'
    $null = Send-Cmd '*(unsigned int*)0x50090000=0x84;'
    $null = Send-Cmd '*(unsigned int*)0x50090014=1;'
    $null = Send-Cmd '*(unsigned int*)0x5000b064=0x3f3f143f;'
    $null = Send-Cmd 'msh();'
    $null = Send-Cmd 'pin mux 20 5'
    $null = Send-Cmd 'pin status 20'
    $null = Send-Cmd 'exit'
    $null = Send-Cmd '*(unsigned int*)0x50090020=0x10;'
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $null = Send-Cmd '*(unsigned int*)0x50090000=0x85;' 70
    $null = Send-Cmd '*(unsigned int*)0x50090024;' 100
    $null = Send-Cmd '*(unsigned int*)0x50090024;' 100
    $null = Send-Cmd '*(unsigned int*)0x50090018;' 100
    $null = Send-Cmd '*(unsigned int*)0x50090038;' 100
    $remaining = 2000 - $timer.ElapsedMilliseconds
    if ($remaining -gt 0) { Start-Sleep -Milliseconds $remaining }
    $null = Send-Cmd '*(unsigned int*)0x50090020=0;' 70
    $null = Send-Cmd '*(unsigned int*)0x50090000=4;' 70
    [void]$log.AppendLine('PWM_OFF; host duration ms: ' + $timer.ElapsedMilliseconds)
} finally {
    if ($port.IsOpen) {
        try {
            # Stop timer/output, then restore GPIO and board startup state.
            $port.Write("*(unsigned int*)0x50090020=0;`r`n")
            Start-Sleep -Milliseconds 80
            $port.Write("*(unsigned int*)0x50090000=4;`r`n")
            Start-Sleep -Milliseconds 80
            $port.Write("msh();`r`n")
            Start-Sleep -Milliseconds 80
            $port.Write("pin mux 20 0`r`npin mode 20 0`r`npin write 20 0`r`npin status 20`r`n")
            Start-Sleep -Milliseconds 300
            [void]$log.Append($port.ReadExisting())
            $port.Write("exit`r`n")
            Start-Sleep -Milliseconds 80
            $port.Write("*(unsigned int*)0x500ca05c=0x000a2b19;`r`n")
            Start-Sleep -Milliseconds 80
            [void]$log.Append($port.ReadExisting())
            $port.Write("msh();`r`n")
            Start-Sleep -Milliseconds 80
            $port.Write("reboot`r`n")
            Start-Sleep -Milliseconds 1100
            [void]$log.Append($port.ReadExisting())
        } finally { $port.Close() }
    }
    $port.Dispose()
    $log.ToString() | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'pwm-test-powered.log') -Encoding utf8
    $log.ToString()
}
