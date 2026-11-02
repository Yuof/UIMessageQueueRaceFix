# UI Message Queue Race Fix

SKSE plugin for Skyrim Special Edition (SE 1.5.97 and Anniversary Edition runtimes). Fixes a vanilla race in the UI
message queue that frees a menu name while the game still uses it, which later crashes whatever reads that name.

`UIMessageQueue::AddMessage` takes the next of its 64 pooled messages by reading the pool count, assigns the
message's menu name (a `BSFixedString` assignment: acquire the new name, release the one the slot held before) and
then increments the count, all without a lock. The game and plugins call it from job threads as well as the main
thread. Two threads inside at once take the same slot and both release its old name, so a menu name's string-pool
entry ("Fader Menu", "HUD Menu", ...) loses a reference each time until the pool frees it while menus still hold it.
The next reader then gets garbage: a menu-name comparison or hash running off the end of memory, in whichever plugin
or game function happens to read that name. `UIMessageQueue::ProcessCommands`, which empties the pool after each
round of UI processing, also resets the count without a lock.

The plugin runs both functions under one lock. Their prologues and the pool-count instruction are checked at load
(the same code in 1.5.97, 1.6.1170 and 1.7.104); an entry that another plugin already detoured with a jump is chained,
so that detour runs inside the lock. Anything else leaves the game unpatched and says so in
`UIMessageQueueRaceFix.log`, which also names calls that had to wait for another thread (the first 20, then every
1000th). VR is not supported.

## Requirements
- [SKSE64](https://skse.silverlock.org/)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)

## Building
Visual Studio 2022 (C++), CMake 3.24+, [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set.

```
git clone --recurse-submodules https://github.com/Yuof/UIMessageQueueRaceFix
cd UIMessageQueueRaceFix
cmake --preset release
cmake --build --preset release
```

The DLL is `build/Release/UIMessageQueueRaceFix.dll`.

## License
[GPL-3.0-or-later](LICENSE), as required by [CommonLibSSE NG](https://github.com/alandtse/CommonLibVR) (GPL-3.0-or-later
with its modding exceptions), which this plugin links statically.
