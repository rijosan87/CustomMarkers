# Licensing

This plugin is licensed **GPL-3.0-or-later**. The full text is in `COPYING` at the
project root.

## Why GPL

The plugin statically links [CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG)
6.7.1, which is GPL-3.0-or-later. Support for Skyrim 1.7.99 (runtime classification and
the format-5 address library) exists only in NG 6.x; the previous pin was NG 3.7.0, which
was MIT. There is no non-GPL route to 1.7.99 - powerof3/CommonLibSSE and
libxse/commonlib-shared are GPL as well.

Distributing the built DLL therefore means distributing a GPL combined work, and the
corresponding source must be made available.

## Third-party components

| Component | License |
|---|---|
| CommonLibSSE-NG 6.7.1 | GPL-3.0-or-later |
| Dear ImGui | MIT |
| PolyHook 2 | MIT |
| DirectXTex | MIT |
| nlohmann/json | MIT |
| lunasvg 3.5.0 | MIT |
| plutovg (lunasvg dependency) | MIT |
