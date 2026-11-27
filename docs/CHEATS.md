# Cheats

Six cheats and two item actions for CUSA57767 1.00 and 1.01. Every cheat is Off by default, including after an upgrade of
the port: a missing key in the settings file means Off.

| Group | Row | Values | Saved key |
| --- | --- | --- | --- |
| PLAYER | God Mode | Off / On | `cheat_god_mode` |
| PLAYER | Infinite Spartan Spirit | Off / On | `cheat_infinite_spirit` |
| PLAYER | Infinite Magic | Off / On | `cheat_infinite_magic` |
| COMBAT | Damage Multiplier | Off, 2x, 4x, 6x | `cheat_damage_multiplier` (1, 2, 4, 6) |
| MOVEMENT | Movement Speed | Off, 2x | `cheat_movement_speed` (1, 2) |
| MOVEMENT | Jump Height | Off, 2x, 4x, 6x | `cheat_jump_height` (1, 2, 4, 6) |
| ITEMS | Add 1000 Blood Orbs | F10 action | none |
| ITEMS | Add 10 Upgrade Materials | F10 action | none |

## Saved values and the session

The launcher's **Cheats** page (after Gameplay in the sidebar) stores the keys above in `user\sons_of_sparta.ini` like any
other setting; Reset puts them back to Off. They are the defaults of the next game. The item actions need the game running
and a player, so they are only in the F10 menu; the page says so and stores nothing for them.

**F10** opens and closes the Cheats menu over the game, in the top right corner, or just below the frame counter when that
counter is shown there; the gap is half the menu's margin, so both follow the font size, scale and resolution. Up/Down or
W/S, the D-pad or the left stick move, Enter, Space, A/Cross or a click change a row (a toggle flips, a multiplier moves to
its next value, an action runs once), F10, Escape or B/Circle close it. Keyboard, mouse and controller share one selection,
and a step scrolls the selected row into view. Controller buttons come from SDL gamepad events of every connected pad
(`sos::cheats::PadState`, `sos::menu::PadNav`): a press acts once however long it is held, a button already held when the
menu opens is ignored until released, and holding up or down repeats after 0.4 s every 0.12 s. A controller unplugged
during the release wait no longer counts as held. F11 still switches the window mode. While the menu is open, and until every
key and button pressed in it is released (one second at most), the game reads neutral input; buttons still held after that
stay hidden from the game until released (`runtime_pad.c`), so neither F10 nor the closing press reaches the game. The menu
does not open over the start-up loading screen.

A change in F10 is a session override: the effective value is the override when there is one, else the saved value
(`sos::cheats::Session`). Changing a row back to its saved value drops the override. Nothing in the menu writes the
settings file, and the next game starts with no overrides.

The run log has one line when the session starts, `Cheats: off` or `Cheats: active God Mode, Damage Multiplier 2x`, one line
per F10 change (`Cheats: Jump Height 4x (this session)`), and `Cheats: used this session` the first time a cheat really
turns On or an item is added. That flag (`runtime_cheats_used`) never goes back to false in the process; nothing uses it yet
and trophies are not blocked.

## Architecture

| Part | File |
| --- | --- |
| The list, keys, labels, choices, saved-value parser, session layer and the rules each hook applies | `src/sos_cheats.h` |
| The F10 menu model (rows, headings, selection) | `src/sos_cheat_menu.h` |
| One-instruction detours: verification, stubs, atomic install and restore | `src/sos_cheat_patch.c`, `.h` |
| The site tables of both builds, generated | `src/sos_cheat_sites.h` by `scripts/cheat_sites.py generate` |
| Runtime state, handlers and item actions | `src/runtime_cheats.cpp`, `.h` |
| F10 menu | `gpu/shim/dcp_overlay_generic.cpp` |
| Launcher page and saved settings | `launcher/launcher.cpp`, `launcher/settings.cpp` |

The loader verifies every site and game function and builds the stubs before the image is made executable
(`runtime_cheats_install`), then applies the saved values just before the game's code runs (`runtime_cheats_start`).

## Patches

A cheat changes the game's code only while it is On. Each site is one game instruction (5 to 7 bytes) inside one aligned
16-byte block. Turning a cheat On replaces it with `jmp rel32` to its stub; Off writes the original bytes back. Each write is
a single locked `cmpxchg16b` of that block that also checks the block still holds the bytes of the other state, so another
thread executing the code sees either the original instruction or the jump, never a mix, and bytes that are neither ours nor
the game's are never overwritten. A thread inside a stub always returns to the instruction after the site, which is the
same in both states.

The stub skips the 128-byte red zone, saves the flags, every general register and ymm0 to ymm15, calls the handler with a
pointer to them, restores them (with the handler's changes), runs the original instruction (rip-relative operands and
`call rel32` are relocated) and jumps back. A handler can also skip the original instruction when it did that work itself.

A cheat's sites go in and out together; a site that cannot be written rolls the earlier ones back. A cheat whose restore
fails is reported **Failed**, never Off, and has no effect (its handlers check the cheat's state first); pressing it again
retries the restore. A site, method or function that does not match the analysed bytes makes that cheat **Unavailable** and
nothing is written for it.

### Identification

`scripts/cheat_sites.py` finds every site independently in each build, from the build's own boot image and IL2CPP metadata:
the method by type, name and full signature, the instruction by a byte pattern that must match exactly once in the method,
a linear sweep that proves it is one whole instruction no branch lands inside, the call targets it relies on, the 16-byte
alignment, and no relocation in the method. It also reads every field offset the handlers use back from the metadata
(`Vitals.maxHp` 0x38, `currentHp` 0x40, `currentGreyHp` 0x44, `SpartanSpirit` 0x20/0x24, `ManaState` 0x28/0x2c,
`HitEventData` 0x10/0x20/0x28/0x30, `DamageConfig.damage` 0x10 and `unmodifiableDamage` 0x35, `MoveStateAerialBase.context`
0x58, `PlayerWallet.walletCount` 0x20, `UnityEngine.Object.m_CachedPtr` 0x10) and refuses a build where one differs,
finds `il2cpp_string_new` by its export NID, and requires every item id in the asset files the build loads (1.01 keeps the
base's `resources.assets` and `sharedassets*`). The header holds, per build, each site's offset, original bytes and the
FNV-1a fingerprint of its whole method, and each called function's range and fingerprint; the image size must match too.
Nothing from the PS5 port's offsets was reused: the PS4 builds have a different code layout and different fields
(`PlayerMovement.fallingCollider` is 0x178 here, 0x168 on PS5).

| Site | Method | 1.00 | 1.01 | Instruction | Handler |
| --- | --- | --- | --- | --- | --- |
| God Mode HP | `Vitals::OffsetHp(float)` | `0x477f226` | `0x47836c6` | `vaddss xmm2, xmm0, [rbx+0x40]` (where every path adds the change) | player: HP = max, grey 0, a negative change 0 |
| God Mode grey | `Vitals::OffsetGreyHp(float)` | `0x477f8d7` | `0x4783d77` | `vaddss xmm0, xmm0, [rbx+0x44]` | player: HP = max, grey 0, a positive grey change 0 |
| Spirit | `SpartanSpirit::OffsetSpartanSpirit(float, bool)` | `0x4773e15` | `0x47782b5` | `vaddss xmm0, xmm1, [rbx+0x20]` | player: current = max, a loss 0 |
| Magic | `ManaState::OffsetMana(float, bool)` | `0x476327a` | `0x476771a` | `vmovss [rbx+0x28], xmm0` (the store of the new value) | player: the stored value is the maximum |
| Damage | `Hittable::ExecuteHitEvent` | `0x475e126` | `0x47625c6` | `call [rax+0x178]`: `hitConfig.damageModule.ApplyModule(eventModuleArgs)` | multiplies the config's damage for this call, restores it |
| Movement | `PlayerMovement::TickPseudoPhysics` | `0x490aca6` | `0x49114d6` | `call GetSlopeAlignedVersionOfMovementDelta` with `deltaFromMovementInputsThisFrame` | the player's input delta times 2 |
| Jump | `MoveStateJump.<DelayStartJumping>d__27::MoveNext` | `0x48feea3` | `0x49056d3` | the instruction after `call GetJumpAngleAsForwardDirectionVector` (no branch reaches it) | the player's upward direction times sqrt(factor) |
| Item actions | `PlayerController::Update` (empty) | `0x40c5610` | `0x40c9ab0` | `ret` and its padding | runs a queued action on player 1's controller |

`HitBasedVitals::OffsetHp` ends in a jump to `Vitals::OffsetHp`, so the one site covers both.

## The cheats

**God Mode**: the green bar is current HP minus grey health. While the player's `Vitals` changes HP, the hook sets HP to its
maximum and grey health to 0, and a negative change becomes 0; healing and the game's other events still run, and the game
clamps HP to its maximum after. A grey-health increase becomes 0. Only a `Vitals` whose GameObject the game's own
`Utilities.PawnUtils.IsPlayer` reports as a player is changed; enemies take their damage.

**Infinite Spartan Spirit**: at `OffsetSpartanSpirit`'s add, the player's meter is set to its maximum and a loss becomes 0;
a gain still goes through (and the game clamps it).

**Infinite Magic**: the value `OffsetMana` stores becomes the player's maximum, so a cost takes nothing and a gain leaves it
full.

**Damage Multiplier**: only a hit whose source GameObject is the player and whose target is not a player, with a nonzero
finite damage the game lets modifiers change (`unmodifiableDamage` false) and a finite product, is multiplied. The damage
config is shared by every hit of that attack, so it is multiplied for the `ApplyModule` call only and restored right after; a
hit raised inside that call is not multiplied again. Damage the player takes, environment damage and self damage are
unchanged.

**Movement Speed**: the hook wraps the slope alignment of the frame's input movement delta in
`PlayerMovement.TickPseudoPhysics`; gravity, knockback, scripted movement and root motion are added to the position after
it, so they are unchanged.

**Jump Height**: a jump's height grows with the square of its upward speed, so the jump direction returned by
`GetJumpAngleAsForwardDirectionVector` (whose product with the jump magnitude is the jump's impulse) has its upward
component scaled by the square root of the factor: 2x, 4x and 6x are about twice, four and six times as high. The PS4 jump
coroutine is the same as the PS5 one here (the same call, the same `forwardAdjustmentFactor` 0x9c and `context` 0x58 fields),
so the same rule applies. Downward impulses and other characters are unchanged; scripted launches do not go through the
coroutine.

**Add 1000 Blood Orbs** and **Add 10 Upgrade Materials**: a press queues the action and hooks the empty
`PlayerController.Update`; the game thread runs it on player 1's controller (`get_PlayerIndex` 0), then the hook is removed.
It finds the player's pawn and wallet (`Controller.GetPawnObject`, `PlayerRuntimeDataUtils.TryGetWallet(GameObject)`),
resolves every item by id (`PlayerInventory.GetItemDefinitionByItemId`) and checks every count before adding anything, then
adds through `PlayerWallet.AddToWallet`: +1000 of `loot_orb_red` (announced as a pickup) or +10 of each of
`loot_common_shield`, `loot_rare_shield`, `loot_epic_shield`, `loot_common_spear`, `loot_rare_spear`, `loot_epic_spear_grip`,
`loot_epic_spear_tail`, `loot_epic_spear_tip` and `loot_epic_spear_tip_status` (no notice). The `loot_boss_*` trophies are
never added. Totals are never written. The row shows `+1000` or `+10`, `Busy` while an action waits, and `Unavailable` when
no player took it within a second or an item or the wallet was missing; nothing is added then. The window thread never
calls a game function.

## Issue #7: not implemented

**Blood Orb Multiplier**: the wallet's one mutator, `PlayerWallet.OffsetWallet`, is called by 15 paths, among them the save
load (`AddWalletItemFromSerializedInstance`), entitlements, vendors, crafting and rewards, so a hook there would multiply
loaded saves. Orbs collected in the world take their own path, `OrbBehaviour.OnAttractEnd`, which switches on the orb type
and tail-calls `EventModule_WalletTransaction.AddAmountToGameObjectsWallet(owner, item, DynamicInt amount, ...)`. That call
is separate from load, spending and the HUD, but the amount is a `DynamicInt` whose layout was not proved, and Blood Orbs
also arrive through `OrbReward`, `WalletReward` and The Pit without it. Not implemented.

**Infinite Items**: Sons of Sparta has no consumable items with a count. The usable item is the healing tool, a meter
(`HealingToolState`, current at 0x30, max at 0x34). Its decrement path `OffsetCurrentAmount(float)` has one direct caller,
`EventModule_Heal.HealTarget`, which also serves healing orbs, and `EventModule_OffsetHealingTool` changes the meter on a path
that was not traced. Without a proved use-only decrement in both builds, nothing was implemented.

## Tests

`tools\test.ps1 -Filter cheats` runs `tools\test_cheats.ps1`: the model and rules, the menu model, the detour engine, and the
real handlers executed through real stubs on synthetic code that holds each site's own instruction bytes (God Mode on the
player and on an enemy, grey health, spirit, magic, every damage factor with enemy, environment, self, zero, NaN and
infinity hits and the restored config, movement, jump factors, the item actions with player 2, a missing player, a missing
item, a full count, Busy and a timeout), every register, the flags and the red zone kept across a hook, Failed restores,
refusals (changed byte, changed method, wrong RVA, other build, unknown build) with nothing written, and the settings file
untouched by F10. On each real boot image every site and function verifies, a changed byte, a wrong RVA and the other
build's table are refused, and with every cheat On only the site bytes differ while every cheat Off is the image byte for
byte. `scripts/cheat_sites.py check` compares the header with both boot images; it and the test report `NO DATA` for a build
without a local image instead of failing. The menu tests place it below the frame counter at several scales, drive every
row with the D-pad, the left stick and A/Cross through the runtime (each press acts once, a held direction repeats at the
set rate), close it with B/Circle and check the release gate, also with a controller unplugged. 37 mutated copies must fail. The build parity gate registers the tables.

## Not covered

What the game does around the hooks was read statically, not observed: whether every damage to the player goes through
`OffsetHp` (scripted kills, instant-death pits and resets may set HP directly), whether the movement states use the input
delta for everything the player steers (ledge, wall and aerial states), the feel of 6x jumps against ceilings and level
geometry, whether materials should include other `loot_*` items the PS4 vendors use, and whether the red-zone or CPU
compatibility passes leave every site in place on every host (if not, the log names the cheat as Unavailable). These are
what the real game test should watch.
