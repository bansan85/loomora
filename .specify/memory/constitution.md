<!--
Sync Impact Report (temporaire, à retirer avant le commit)
Version : 1.0.0 → 1.0.1 (PATCH : correction du principe IX, builds par plateforme)
Principe modifié : IX. Builds (liste par plateforme). Aucun ajout ni retrait.
TODO différés : aucun.
-->

# Constitution Loomora

## Core Principles

### I. Backend unique en C++11
Le backend est compilé en C++11 (propriété de cible, jamais global), sans seconde implémentation. UI Legacy en C++11, UI Modernes jusqu'à C++17.

### II. Une seule frontière : l'API C
Aucune logique dans les UI. Pas de type C++ ni d'exception dans l'en-tête ; les données passent par des tampons de l'appelant. L'API est additive tant que `LOOMORA_ABI_VERSION` n'est pas changé par le propriétaire.

### III. Replis par cas
Un repli ne vise que la plateforme, le compilateur ou la version fautive, jamais le plus restrictif pour tous. Chacun est déclaré dans `validation/fallbacks.json`.

### IV. Tests d'abord
Les tests sont écrits avant le code. `python tools/verify.py quick` vérifie toute branche ; `full` seulement pour un changement graphique ; pas de capture pour vérifier que ça marche.

### V. Preuves sobres
Une preuve n'est stockée que si elle montre du nouveau. Aucune image sans accord du propriétaire.

### VI. Flux Git
`master` ne contient que des merges `--no-ff`. Tout changement est sur une branche dédiée. Aucun merge ni déplacement du pointeur du sous-module docs sans accord explicite du propriétaire.

### VII. Commits
En anglais, Conventional Commits, titre de 50 caractères au plus, impératif.

### VIII. Documentation d'implémentation
ADR, validation et évidence vivent dans le sous-module `docs/implementation` ; les specs Spec Kit dans `specs/`. Le build n'a jamais besoin du sous-module.

### IX. Builds
Linux Legacy et Modern : compilés depuis WSL. Windows Legacy : cross-compilé avec MinGW-w64 depuis WSL ; Windows Modern : avec Visual Studio. Apple : compilé depuis les VM VirtualBox. WebAssembly : depuis WSL avec Emscripten. Tout build WSL est sous `$HOME`.

### X. Outils hôtes
Python 3.8+ avec la bibliothèque standard (`jsonschema` accepté).

## Governance

Ces principes priment sur toute autre pratique. Toute dérogation exige un ADR (raison et alternative plus simple écartée). Amendement : modification de ce fichier validée par le propriétaire, version sémantique (MAJOR retrait ou redéfinition d'un principe, MINOR ajout, PATCH reformulation). Chaque revue de plan ou d'analyse vérifie la conformité.

**Version**: 1.0.1 | **Ratified**: 2026-11-17 | **Last Amended**: 2026-11-17
