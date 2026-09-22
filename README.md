# CreativeSystem Atlas alpha 0.1


[AI made Until I get better at Coding] Actuellement en Raster mais va evolué sur un logiciel d"illu vectoriel simplifier (car j'en ai marre des logiciels de vectoriel pas du tout intuitif)
A servie de labo pour améliorer les perf de Nebula mais il est temps a Atlas a terme de rejoindre son vrais objectif 
MAJ Majeur fin de semaine pour la réorientation total de l'application




Première base modulaire de CreativeSystem : moteur C++ indépendant de l’interface,
TileStore, cache LRU, FusionCreatorEngine et point d’entrée prévu pour GTK 4 /
Wayland / EGL. Le renderer GPU sera branché après validation du modèle de données.

```text
UI GTK4/Wayland → API stable → CreativeCore C++ → OpenGL/EGL ou CPU fallback
```

## État de l’alpha

- `BrushEngine`, `TileStore`, cache LRU, registre de fusions et pool de threads C++ sont présents.
- Le dessin souris alimente le moteur C++ et le buffer RGBA est envoyé vers une texture GL native persistante.
- Une API C version 1 est réservée aux plugins et extensions.
- Le fallback Cairo/CPU reste actif pour la présentation finale pendant la stabilisation du présentateur GPU.
- Le contexte EGL de l’alpha est unique et hors écran ; la présentation directe sur surface Wayland reste une étape dédiée.
- Le GPU peut être désactivé explicitement avec `CREATIVE_DISABLE_GPU=1` pour tester le fallback CPU.

===================En Cours===================
Réorientation d'Atlas vers sont but premier : Logiciel d'illustration vectorisé simplifié
Conserver les perfomance et la gestion mémoire/ram d'Atlas
Garder GTK4 ? 
Bref pleins de truc 
