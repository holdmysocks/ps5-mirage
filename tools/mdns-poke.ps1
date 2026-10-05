# PC side of the mDNS probe (probe\mdnsprobe.c).
#   .\tools\mdns-poke.ps1 -PS5Host <console-ip>            # stage 1: unicast datagrams to the console's port 5353
#   .\tools\mdns-poke.ps1 -PS5Host <console-ip> -Multicast # stage 2: the same query to 224.0.0.251
# Sends a standard mDNS question (PTR _mirage-probe._udp.local) once a second.
param(
    [Parameter(Mandatory = $true)][string]$PS5Host,
    [switch]$Multicast,
    [int]$Count = 10
)

$name = [byte[]](@(13) + [Text.Encoding]::ASCII.GetBytes('_mirage-probe') + @(4) +
        [Text.Encoding]::ASCII.GetBytes('_udp') + @(5) + [Text.Encoding]::ASCII.GetBytes('local') + @(0))
$query = [byte[]](@(0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0) + $name + @(0, 12, 0, 1))
$target = if ($Multicast) { '224.0.0.251' } else { $PS5Host }

$udp = [Net.Sockets.UdpClient]::new()
try {
    if ($Multicast) {
        # Leave through the interface that routes to the console, not whichever Windows prefers.
        $route = [Net.Sockets.UdpClient]::new()
        $route.Connect($PS5Host, 9)
        $local = $route.Client.LocalEndPoint.Address
        $route.Close()
        $udp.Client.SetSocketOption([Net.Sockets.SocketOptionLevel]::IP,
            [Net.Sockets.SocketOptionName]::MulticastInterface, $local.GetAddressBytes())
        $udp.Client.SetSocketOption([Net.Sockets.SocketOptionLevel]::IP,
            [Net.Sockets.SocketOptionName]::MulticastTimeToLive, 255)
        Write-Host "[multicast via $local]"
    }
    for ($i = 1; $i -le $Count; $i++) {
        [void]$udp.Send($query, $query.Length, $target, 5353)
        Write-Host ("[{0}/{1}] {2} bytes -> {3}:5353" -f $i, $Count, $query.Length, $target)
        Start-Sleep -Seconds 1
    }
} finally {
    $udp.Close()
}
