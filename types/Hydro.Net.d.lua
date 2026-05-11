---@meta

--- @module Hydro.Net
--- @description Network role detection and modpack identity for multiplayer mods.
---   Foundational hooks every multiplayer-aware mod needs:
---     Net.isHost()         - am I the authoritative host?
---     Net.mode()           - what role am I playing?
---     Net.getModpackHash() - deterministic identity for the active mod set
---
---   Does NOT yet provide custom RPCs (send/broadcast/on). Those land once
---   the per-game replicated router asset is available. For now, mod-to-mod
---   network sync goes through the game's existing replicated actors -
---   `Hydro.Events.hook()` already catches network-replicated UFunction
---   calls regardless of dispatch path.
---
---   **Security note:** in P2P listen-server games (e.g. SN2), the host has
---   full authority and can arbitrarily lie to clients. UE replication is a
---   transport, not a trust boundary. Validate joiner-supplied data on the
---   host; assume host-supplied data is authoritative.
--- @depends EngineAPI (UWorld::GetNetMode), Manifest
--- @engine_systems UWorld

local Net = {}

--- Returns true if this process is the authoritative host.
---
--- True for `listen_server` (P2P host) and `dedicated_server`. False for
--- `client`, `standalone`, and `unknown`.
---
--- **Timing:** at top-level mod init the game has no active session and
--- this returns false. Query from a `BeginPlay` hook (or any later event)
--- so the network mode has been established by the engine.
---
--- @return boolean isHost
--- @engine UWorld::GetNetMode
---
--- ```lua
--- local Net    = require("Hydro.Net")
--- local Events = require("Hydro.Events")
---
--- Events.hook("/Script/Engine.Actor:ReceiveBeginPlay", function(self)
---     if Net.isHost() then
---         -- authoritative logic: spawn, modify world state
---     end
--- end)
--- ```
function Net.isHost() end

--- Returns the current network role of this process.
---
--- One of:
---   - `"standalone"` - singleplayer or main menu, no network session
---   - `"listen_server"` - this process is hosting a P2P session
---   - `"dedicated_server"` - this process is a headless dedicated server
---   - `"client"` - joined a remote session
---   - `"unknown"` - discovery failed (treat as `"standalone"`)
---
--- Returns `"standalone"` until a session is established. Same timing
--- caveat as `isHost()` - query from `BeginPlay` or later.
---
--- @return string mode
--- @engine UWorld::GetNetMode
---
--- ```lua
--- local Net = require("Hydro.Net")
--- if Net.mode() == "client" then
---     -- joiner-only logic: cosmetics, UI, prediction
--- end
--- ```
function Net.mode() end

--- Returns a deterministic hex string identifying the current active modpack.
---
--- Computed as FNV-1a 64 over the sorted `mod_id:version_id` list of all
--- enabled mods. Two players with identical enabled mod sets at identical
--- versions produce the same 16-char hex string regardless of install order.
---
--- Use for informal modpack compatibility checks (e.g. log host's hash on
--- session start, log joiner's hash on connect, surface mismatch in UI).
--- This is **not** cryptographically secure - it's a content-addressable
--- identity, not a signature. A malicious peer can trivially fake it.
---
--- Returns `"0000000000000000"` if no manifest is loaded.
---
--- @return string hash
---
--- ```lua
--- local Net = require("Hydro.Net")
--- print("Modpack: " .. Net.getModpackHash())
--- ```
function Net.getModpackHash() end

return Net
