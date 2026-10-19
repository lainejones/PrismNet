# PrismMon

PrismMon is a window that shows what PrismNet is doing. It refreshes every
second.

## Buttons

- **Start** and **Stop** start and stop PrismNet.
- **Refresh** updates the view now.
- **Prefs...** opens PrismNet Prefs.

## Tabs

The keys **1** to **5** switch between them.

| Tab | Shows |
|---|---|
| Status | running or not, the DNS servers, each interface's address and link, and the memory in use |
| Interfaces | each interface: driver and unit, address, netmask, gateway, hardware address, MTU, speed, packets and bytes in and out, drops and errors |
| Connections | every open socket: protocol, local and remote address, state and the program that owns it |
| Memory | what the stack uses: program, stack, lwIP memory against the budget, packet buffers, TCP and UDP pools, refused allocations, and the machine's free memory |
| Log | PrismNet's recent messages (the same as `PrismNet LOG`) |

Right-click a line for **Copy line**, **Copy address** (to the clipboard)
and **Refresh**.

The **Project** menu has *About...* and *Quit*.
