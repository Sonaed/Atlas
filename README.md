# CreativeSystem Atlas alpha 0.1

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
