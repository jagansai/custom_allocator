# run the server in the background

param(
    [int]$Runs = 10
)

# run the server in the background and collect per-run stats

$logDir = ".\logs\allocator"
$runLog = Join-Path $logDir "run.log"
if (Test-Path $runLog) {
    Remove-Item $runLog -Force
}

$results = @()

for ($i = 1; $i -le $Runs; $i++)
{
    "Run #$i" | Out-Host
    Start-Process -FilePath ".\build-release\Release\fix_allocator_demo.exe" -ArgumentList "--mode server --server-config .\config\app.properties --output-dir .\logs\allocator"

    # wait a few seconds to ensure the server starts
    "Waiting for server to start..." | Out-Host
    Start-Sleep -Seconds 1
    
    # get the process ID of the server
    $serverProcess = Get-Process -Name "fix_allocator_demo" -ErrorAction SilentlyContinue
    if ($null -eq $serverProcess) {
        Write-Error "Server process not found. Please ensure the server is running."
        exit 1
    }

    # check if the command is successful
    "Server started and sending messages" | Out-Host
    # run the message sender, capture output, and append to run.log
    $output = python .\scripts\send_fix_messages.py --config .\config\app.properties --file .\data\fix_messages.txt --log-dir $logDir 2>&1 | Tee-Object -FilePath $runLog -Append

    # parse which allocator was sent first in this run
    $firstConnectLine = $output | Where-Object { $_ -match '^Connecting to .*session at' } | Select-Object -First 1
    if ($firstConnectLine -and $firstConnectLine -match 'arena session') {
        $firstSent = 'Arena'
    } elseif ($firstConnectLine -and $firstConnectLine -match 'heap session') {
        $firstSent = 'Heap'
    } elseif ($firstConnectLine -and $firstConnectLine -match 'pool session') {
        $firstSent = 'Pool'
    } else {
        $firstSent = 'Unknown'
    }

    # parse Arena/Heap/Pool timing lines
    $arenaLine = $output | Where-Object { $_ -match '^Arena:' } | Select-Object -Last 1
    $heapLine  = $output | Where-Object { $_ -match '^Heap:' }  | Select-Object -Last 1
    $poolLine  = $output | Where-Object { $_ -match '^Pool:' }  | Select-Object -Last 1

    $arenaMessages = $null
    $arenaTime = $null
    $heapMessages = $null
    $heapTime = $null
    $poolMessages = $null
    $poolTime = $null

    if ($arenaLine -and $arenaLine -match 'Arena:\s+(\d+)\s+messages\s+over\s+([0-9.]+)\s+seconds') {
        $arenaMessages = [int]$matches[1]
        $arenaTime = [double]$matches[2]
    }

    if ($heapLine -and $heapLine -match 'Heap:\s+(\d+)\s+messages\s+over\s+([0-9.]+)\s+seconds') {
        $heapMessages = [int]$matches[1]
        $heapTime = [double]$matches[2]
    }

    if ($poolLine -and $poolLine -match 'Pool:\s+(\d+)\s+messages\s+over\s+([0-9.]+)\s+seconds') {
        $poolMessages = [int]$matches[1]
        $poolTime = [double]$matches[2]
    }

    if ($arenaTime -ne $null -and $heapTime -ne $null) {
        # Determine overall winner among arena, heap, and optional pool
        $winner = 'Tie'
        $bestTime = $arenaTime
        $winner = 'Arena'
        if ($heapTime -lt $bestTime) {
            $bestTime = $heapTime
            $winner = 'Heap'
        }
        if ($poolTime -ne $null -and $poolTime -lt $bestTime) {
            $bestTime = $poolTime
            $winner = 'Pool'
        }

        $messages = if ($arenaMessages -ne $null) { $arenaMessages }
                    elseif ($heapMessages -ne $null) { $heapMessages }
                    else { $poolMessages }

        $obj = [pscustomobject]@{
            Run        = $i
            Messages   = $messages
            FirstSent  = $firstSent
            ArenaTimeS = [math]::Round($arenaTime, 3)
            HeapTimeS  = [math]::Round($heapTime, 3)
            Winner     = $winner
        }

        if ($poolTime -ne $null) {
            Add-Member -InputObject $obj -NotePropertyName PoolTimeS -NotePropertyValue ([math]::Round($poolTime, 3))
        }

        $results += $obj
    }

    # after python script ends, stop the server
    Stop-Process -Id $serverProcess.Id  
    "Server stopped." | Out-Host
}

if ($results.Count -gt 0) {
    "" | Out-Host
    "Summary (per run):" | Out-Host
    if ($results[0].PSObject.Properties.Name -contains 'PoolTimeS') {
        $results | Format-Table Run, Messages, FirstSent, ArenaTimeS, HeapTimeS, PoolTimeS, Winner -AutoSize | Out-Host
    } else {
        $results | Format-Table Run, Messages, FirstSent, ArenaTimeS, HeapTimeS, Winner -AutoSize | Out-Host
    }
}



