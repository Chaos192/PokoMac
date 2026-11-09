PokoMac @VERSION@ for Linux, SteamOS and the Steam Deck

1. In Steam, install Proton Experimental (Library, Tools; on a Steam Deck switch to Desktop Mode first).
2. Run PokoMac.AppImage and pick your dump folder (base/ and updates/<version>/, each with exefs/ and romfs/) or a .nup. Version 2.0.1a is required.
3. Press Set up. The first time it downloads its build tools (Python and Zig, about 100 MB), builds pkp.exe and the shader cache on this PC, through Proton. You can pause and continue later.
4. Press Play. The game runs through Steam's Proton.

If PokoMac.AppImage does not start (a system without FUSE): ./PokoMac.AppImage --appimage-extract-and-run
From Steam (Game Mode on the Deck): add PokoMac.AppImage as a non-Steam game with the launch options --play.

Updating: extract the new release over your install. If the update changes the game code, the launcher says so: press Set up again and it rebuilds pkp.exe. Your shaders and saves are kept.

Licences: LICENSE.txt and licenses/.
