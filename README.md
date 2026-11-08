# Raccord

Logiciel de montage vidéo professionnel pour Linux, écrit de zéro en C++20 / Qt 6 / OpenGL / FFmpeg.
Interface, raccourcis et échanges de projets pensés pour les monteurs qui viennent de Premiere Pro.

## Fonctions

- **Timeline** multipiste (V/A/sous-titres), outils Premiere : sélection, sélection de piste, propagation, modification
  par déplacement, extension, cutter, glissement, débordement, plume, main, zoom, texte. Insertion/écrasement,
  suppression avec raccord, magnétisme, liaison A/V, imbrication, marqueurs, points d'entrée/sortie, J/K/L.
- **Formats** : tout ce que lit FFmpeg (H.264, HEVC 10 bits, ProRes, DNxHR, VP9, AV1, images, séquences), toutes
  résolutions et cadences, rotation des smartphones, alpha. Temps interne exact en ticks (unité Premiere).
- **Proxies** H.264 générés en tâche de fond, bascule automatique.
- **Rendu GPU** (OpenGL 3.3) : 40 effets vidéo, 18 transitions vidéo, masques animés, incrustation (Ultra Key),
  stabilisation, LUT .cube, titres animés, modes de fusion. Images clés avec interpolations (linéaire, Bézier, maintien).
- **Étalonnage** façon Lumetri : correction de base, courbes RVB et teinte, roues chromatiques, TSL secondaire,
  vignette ; moniteurs forme d'onde, parade RVB, vectorscope, histogramme.
- **Audio** : 15 effets, 3 fondus, table de mixage avec vumètres, panoramique, mute/solo,
  normalisation de sonie EBU R128.
- **Assistant IA** (local) : sous-titres automatiques avec Whisper (faster-whisper), suppression des silences,
  détection de plans, recadrage automatique (vertical/carré), stabilisation.
- **Export** : 29 préréglages (YouTube, réseaux sociaux, ProRes, DNxHR, H.265, AV1, GIF, audio, séquence d'images),
  réglages complets, file de rendu, export des sous-titres (SRT/VTT).
- **Échange** : import/export FCP7 XML (le format d'échange de Premiere Pro), EDL CMX3600, OpenTimelineIO, export
  FCPXML, import direct des projets `.prproj`.
- **Interface** en français et en anglais, espaces de travail (Montage, Couleur, Audio, Effets, Assemblage),
  enregistrement automatique et récupération après plantage.

## Installation

Les versions publiées (paquet `.deb` et AppImage) sont générées par la CI à chaque tag `v*`.

```sh
sudo apt install ./raccord_0.1.0_amd64.deb      # Debian 12+, Ubuntu 23.04+
chmod +x Raccord-0.1.0-x86_64.AppImage && ./Raccord-0.1.0-x86_64.AppImage
```

Les sous-titres automatiques installent Whisper au premier usage (Assistant > Installer Whisper), il faut `python3-venv`.

## Compiler

```sh
sudo apt install cmake ninja-build g++ pkg-config qt6-base-dev libqt6opengl6-dev qt6-multimedia-dev \
  qt6-l10n-tools qt6-tools-dev-tools libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
  libswresample-dev libavfilter-dev libebur128-dev zlib1g-dev catch2
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DRACCORD_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build                 # tests du moteur
cd build && cpack -G DEB               # paquet .deb
packaging/build-appimage.sh build      # AppImage (nécessite appimagetool)
```

## Ligne de commande

```
raccord-cli probe <média>                        informations (JSON)
raccord-cli presets                              préréglages d'export
raccord-cli frame <projet> <secondes> <out.png>  rendu d'une image
raccord-cli export <projet> <sortie> [préréglage] [séquence]
raccord-cli assemble <out.raccord> <médias...>   projet bout à bout
```

## Traduction

`lupdate src app cli -ts i18n/raccord_fr.ts`, puis `python3 i18n/translate_fr.py <(raccord-cli effect-strings)`
et `lrelease i18n/raccord_fr.ts -qm i18n/raccord_fr.qm`.

## Structure

- `src/core` : modèle (projet, séquences, clips, paramètres animés), annulation, opérations de montage, registre d'effets
- `src/media` : décodage FFmpeg, cache de médias, sondes
- `src/render` : moteur OpenGL, shaders, titres, LUT
- `src/audio` : mixage et effets audio
- `src/playback` : lecture temps réel
- `src/export` : encodage, préréglages, proxies
- `src/ai` : détection de plans/silences, recadrage, stabilisation, Whisper
- `src/interop` : FCP7 XML, EDL, OTIO, FCPXML, .prproj, sous-titres
- `src/ui` : interface Qt
