# Notice

This repository is a **PS5 port kit** for the Motorhead (1998) reconstruction by S95Sedan
(https://github.com/S95Sedan/motorhead, pinned in `tools/pins.env`).

- It contains **no game files** and none of the reconstruction's source: the build downloads the reconstruction
  at the pinned commit and applies the patches and files in `ps5/` on top. You supply your own copy of the game.
- The patches and overlay files are modifications of the reconstruction and are therefore distributed under the
  same **Motorhead Non-Commercial License 1.0** (see `LICENSE`). The notice must be kept, and these changes are
  marked as such: PS5 platform layer, Vulkan scene renderer, GPU sky and fog, lens flares and round lamp glows,
  on-screen keyboard, cinematic camera, music level, quiet log, selected-option highlight.
- Original Motorhead and S40 Racing game assets, trademarks and other third-party material are not covered by that
  license and remain with their owners. No commercial use.
- The PS5 toolchain is third-party and fetched at build time: PS5_Vulkan, PS5_PayloadSDK and PS5_Mesa by
  mihawk-99 (see `tools/pins.env`), SDL3 (zlib license, shipped with the reconstruction).
- Not affiliated with or endorsed by Sony, Activision, Gremlin/Infogrames or the original developers.
