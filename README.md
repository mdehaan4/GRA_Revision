# Coral Bay (C++)

A small Vice City-style 3D game in C++17 with raylib. You play Nico Salazar, an
ex-boxer back in a neon beach city in 1986, with a loan shark called Marco on
your back.

## Build

You need CMake 3.16+, a C++17 compiler and git. raylib is downloaded automatically.

```
cmake -S . -B build
cmake --build build --config Release
```

Run the game **from this folder** so it can find `assets/`:

```
./build/coralbay            (Mac / Linux)
build\Release\coralbay.exe  (Windows)
```

Linux may need: `sudo apt install libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev libasound2-dev`

## Controls

| Key | Action |
| --- | --- |
| WASD | Move / drive |
| Mouse | Look around |
| Shift | Sprint |
| F | Get in, get out, carjack |
| Enter (next to a pedestrian) | Answer an AWS AI Practitioner practice question. +$100 per correct answer |
| Space | Handbrake |
| H | Horn |
| R | Change radio station |
| Tab | Show what every pedestrian and driver is thinking |
| G | Switch between fancy and simple graphics |
| Esc | Pause |

## Adding real 3D models (optional)

The game runs with its own block models, but looks far better with real ones.
All of these are free:

1. **Characters:** download the *Ultimate Animated Character Pack* by Quaternius
   (quaternius.com). Copy a few `.gltf`/`.glb` characters into `assets/characters/`.
   Rename one to include `nico` (the player) and one to include `cop`.
2. **Cars:** download the *Car Kit* from kenney.nl. Copy some `.glb` files
   (sedan, taxi, sports car) into `assets/cars/`, keeping any `Textures` folder
   next to them.
3. **Your own animations:** Mixamo characters work too. Export as FBX, open in
   Blender, and export as glTF (.glb) with animations named Idle, Walk, Run
   and Death.

Models are scaled automatically (people to 1.8 m, cars to 4.4 m). Animations are
matched by name: idle, walk, run, death/die. If models face the wrong
way, change `CHAR_YAW_FIX` or `CAR_YAW_FIX` at the top of `src/main.cpp`.

## What's in the code

- `src/main.cpp`: the game. City map, scenery, model loading, pedestrians, traffic,
  cops, missions, camera, rendering and HUD.
- `src/shaders.hpp`: lighting. A low sunset sun with soft shadows (shadow map),
  sky light, distance fog, and a glow pass that makes neon signs bloom.
- `src/quiz.hpp`: the AWS Certified AI Practitioner (AIF-C01) practice questions pedestrians ask you.
- `src/audio.hpp`: all sound is generated in code. Two synthwave radio stations
  (Neon FM 86.4 and Wave 103), engine noise, distant sirens and sound effects.

### The story

Marco calls a few seconds in. Meet him at the Pink Flamingo beach bar, steal a
gold car and deliver it to his lockup, draw the cops' attention and lose them,
then pay back $1500. After that, Coral Bay is yours to roam.
