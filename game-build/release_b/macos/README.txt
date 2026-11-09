PokoMac @VERSION@ (Apple silicon, macOS 14.4 or newer)

1. Move PokoMac.app to Applications (or any folder you can write in) and open it. The app is signed ad hoc, not by an Apple developer
   account, so the first time macOS asks: right-click the app, choose Open, then Open again (or System Settings > Privacy & Security >
   Open Anyway). In Terminal, `xattr -dr com.apple.quarantine /Applications/PokoMac.app` does the same.
2. Pick your dump folder (base/ and updates/<version>/, each with exefs/ and romfs/). Version 2.0.1a is required.
3. Press Set up. The first time it downloads Python and Zig for macOS (about 80 MB, checked against pinned hashes), then builds the game
   (pkp) and the shader cache on this Mac. You can pause and continue later.
4. Press Play.

Where things are: the game folder is ~/Library/Application Support/PokoMac (Settings can move it): pkp, its runtime and Vulkan files
(copied from the app before every start), pkp.ini, the build, the shader cache and your save. The launcher's own settings are in
~/Library/Application Support/io.github.chaos192.pokomac. The app itself is never changed.

Updating: the launcher updates itself from https://github.com/Chaos192/PokoMac/releases (it replaces the app). If an update changes the
game code, it says so: press Set up again and it rebuilds pkp. Your shaders and saves are kept.

Graphics: Vulkan runs on Metal through KosmicKrisp (the default) or MoltenVK ([gfx] vulkan_driver = moltenvk in pkp.ini). DLSS is not
available on a Mac.

Help and bug reports: https://github.com/Chaos192/PokoMac/issues

Licences: LICENSE.txt and licenses/ (also inside the app, Contents/Resources/licenses).
