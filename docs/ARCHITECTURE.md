# Atlas — documentation technique

Ce document décrit Atlas tel qu'il est dans le code, pour pouvoir le reprendre
seul : ce que fait chaque module, comment les données circulent, où se trouvent
les coûts, et ce qui reste fragile. Quand le document et le code divergent,
c'est le code qui fait foi — corrige alors le document.

Dernière révision : septembre 2026.

---

## 1. Vue d'ensemble

Atlas est un logiciel de dessin raster pour Linux, écrit en C++20, avec une
interface GTK4 native Wayland. Le document est stocké en tuiles de 64×64 px,
allouées à la demande et évincables vers un fichier scratch quand la RAM
manque. Le trait est lissé par une courbe d'Hermite à tangente persistante, et
la pression et l'inclinaison de la tablette sont lues directement depuis les
événements GDK.

Deux couches strictement séparées :

- **CreativeCore** (`engine/`) : bibliothèque statique sans aucune dépendance
  à GTK ni à Wayland. Stockage, mémoire, brosses, calques, sérialisation,
  outils, fusions.
- **Application** (`ui/gtk/`) : interface GTK4, gestion des entrées, affichage
  Cairo, et le chemin GPU optionnel (EGL + GLES2 sur sous-surface Wayland).

La règle qui structure tout le reste : **le moteur compose, l'UI présente.**
Aucun fichier de `engine/` ne présente une image à l'écran.

## 2. Construire et lancer

```bash
cd ~/Documents/CreativeSystemAtlas_alpha0.1
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/CreativeSystemAtlasGtk
```

Dépendances (paquets Arch) : `gtk4`, `libpng`, `libtiff`, `zlib`, `python`
(pour `python3-embed`), `mesa` (EGL, GLESv2, OpenGL), `wayland`.

Variable d'environnement :

| Variable | Effet |
|---|---|
| `CREATIVE_ENABLE_GPU=1` | Tente le rendu GPU. Désactivé par défaut, voir §6. |

Après un changement dans `CMakeLists.txt` qui ajoute ou retire des cibles ou
des bibliothèques, repartir d'un dossier propre (`rm -rf build`) : CMake garde
en cache les résultats de `find_library`.

`app/main.cpp` n'est plus construit. C'est une ancienne copie de l'interface
Atlas, restée figée pendant que `ui/gtk/main.cpp` évoluait. Il peut être
supprimé.

## 3. Arborescence utile

```
CMakeLists.txt              build (CreativeCore + CreativeSystemAtlasGtk)
engine/
  memory/memory_manager.*   budget RAM global, scratch disk
  tiles/tile_store.*        stockage tuilé, pagination, annulation par tuiles
  brush/brush_engine.*      empreintes (dab), segments, traits
  brush/bitmap_brush.*      brosse à partir d'une image
  brush/advanced_brushes.*  brosses spécialisées
  layers/layer_stack.h      modèle de calques générique (raster, groupe…)
  layers/layer_compositor.* composition CPU d'une LayerStack
  render/native_renderer.*  présentation GPU d'un TileStore (GLES2)
  serialization/atlas_project.*   format .atlas, export PNG / TIFF
  tools/tool_system.*       outils, registre, outils scriptés en Python
  fusion/fusion_creator_engine.*  modes de fusion et validation GLSL
  optimization/resource_manager.h pool de tampons, profileur
  cache/lru_cache.h, pool/thread_pool.h, plugin/, projection/
ui/gtk/
  main.cpp                  toute l'application GTK (~1 900 lignes)
  native_canvas_surface.*   sous-surface Wayland + contexte EGL
  color_wheel.*             roue chromatique anneau + triangle
api/                        interfaces publiques (plugins, fusions)
docs/ARCHITECTURE.md        ce document
```

Les dossiers `platform/`, `renderer/`, `shaders/`, `scripting/`, `plugins/`,
`resources/`, `tests/`, `io/`, `model/` sont des emplacements prévus mais vides
ou non utilisés par le build.

## 4. Flux de données d'un trait

C'est le chemin le plus important à comprendre. Du stylet à l'écran :

```
stylet XP-Pen
  │  libinput → Wayland (zwp_tablet_v2) → GDK
  ▼
raw_tablet_event()                     ui/gtk/main.cpp
  │  contrôleur Legacy en phase CAPTURE : reçoit souris ET stylet
  │  lit position, pression, inclinaison (voir §7)
  │  BUTTON_PRESS  → undo_begin(), reset du filtre de pression
  │  MOTION_NOTIFY → ajoute un point, flush_latest_segment()
  │  BUTTON_RELEASE→ undo_commit()
  ▼
flush_latest_segment()                 lissage Hermite (voir ci-dessous)
  ▼
emit_hermite()                         empreintes le long de la courbe
  │  espacement = rayon × brush_space × échelle d'affichage
  ▼
dab_at()                               rayon selon pression et inclinaison
  │  dirty_expand() : agrandit le rectangle modifié
  │  pinceau → BrushEngine::dab()      gomme → boucle locale
  ▼
TileStore::span_for_write()            écriture directe dans la tuile
  │  enregistre la pré-image si une annulation est en cours
  ▼
refresh_view()                         au plus toutes les 14 ms
  │  refresh_composite() : ne recompose QUE le rectangle modifié
  │  gtk_widget_queue_draw()
  ▼
draw_cb()                              Cairo : damier, image, bordure, curseur
```

**Le lissage.** Chaque segment est tracé dès que son point arrive, sans
attendre le suivant : aucun retard sur le curseur, et rien qui surgit au
relâchement. C'est une courbe d'Hermite dont la tangente de départ est la
tangente d'arrivée du segment précédent, ce qui supprime les angles. La
tangente d'arrivée est la corde tournée de la moitié du virage observé entre
les deux dernières cordes (tangente exacte pour un mouvement circulaire) ; les
deux tangentes sont ramenées à la longueur de la corde courante, sinon la
courbe oscille quand la vitesse varie. La tangente est remise à zéro à chaque
trait. Historique : une interpolation linéaire donnait des polygones, un
Catmull-Rom classique un segment de retard, une extrapolation simple des
angles à chaque point.

La composition (`refresh_composite`) repose sur deux tampons au format Cairo
ARGB32 prémultiplié. **Ordre des calques** : l'indice 0 est le calque du
dessus (un nouveau calque est inséré en tête), le dernier indice est le fond.

- `bg_cache` : les calques visibles situés **sous** le calque actif (indices
  supérieurs à l'actif), déjà composés. Reconstruit seulement quand la
  structure des calques change (ajout, suppression, visibilité, opacité, mode,
  changement de calque actif) via `invalidate_bg_cache()`.
- `cbuf` : ce qui est affiché. Pour chaque frame, sur le rectangle modifié
  uniquement : copie de `bg_cache`, puis composition du calque actif, puis des
  calques situés au-dessus, du plus bas au plus haut.

Les calques du dessus ne sont pas précomposés dans un second cache : avec un
mode de fusion autre que Normal, un calque se fusionne avec tout ce qu'il
recouvre, calque actif compris, donc son résultat change à chaque coup de
pinceau.

Conséquence pratique : dessiner ne coûte que la surface peinte, quelle que soit
la taille du document. Le nombre de calques **au-dessus** de l'actif multiplie
ce coût ; les calques en dessous sont gratuits. Si tu ajoutes une opération qui
modifie les pixels d'un calque **inactif**, appelle `invalidate_bg_cache()`,
sinon l'écran ne se mettra pas à jour.

## 5. Les modules du moteur

### 5.1 MemoryManager — `engine/memory/`

Singleton (`MemoryManager::instance()`) qui comptabilise toute la mémoire pixel
des `TileStore` et gère le scratch disk.

- **Budget** : 60 % de la RAM physique par défaut, réglable en pourcentage
  (`set_budget_percent`, 1 à 95 %) ou en octets. Plancher interne 16 Mio.
- **Comptabilité** : chaque allocation ou libération de tuile appelle
  `on_alloc` / `on_free`. `resident_bytes`, `swapped_bytes`, `peak_bytes` et
  `scratch_bytes` alimentent le panneau *Édition → Mémoire et scratch disk*.
- **Scratch** : un fichier créé par `mkstemp` dans le répertoire choisi
  (défaut `~/.cache/atlas`, ou `$XDG_CACHE_HOME/atlas`), puis immédiatement `unlink`é. Il n'existe plus
  dans le système de fichiers mais reste utilisable tant que le descripteur est
  ouvert : il disparaît donc même si Atlas plante. Les blocs libérés vont dans
  une free-list et sont réutilisés à taille égale (toutes les tuiles ont la
  même taille, donc c'est le cas courant).
- Accès protégé par un mutex : utilisable depuis plusieurs threads.

Le défaut n'est volontairement pas `/tmp` : sous Arch c'est un tmpfs, donc de
la RAM, et y évincer des tuiles ne libérerait rien. `/tmp` n'est utilisé qu'en
dernier recours, si `~/.cache/atlas` ne peut pas être créé. Pour les très gros
documents, choisir un répertoire sur le SSD le plus rapide.

### 5.2 TileStore — `engine/tiles/`

Le stockage d'un calque. Canvas découpé en tuiles carrées (64 px dans l'UI),
chacune vide tant qu'on n'y a pas écrit : un document vierge ne coûte presque
rien.

Une tuile est dans l'un de trois états : **absente** (jamais écrite),
**résidente** (pixels en RAM) ou **évincée** (pixels sur le scratch, offset
mémorisé). Le passage de l'un à l'autre est transparent pour l'appelant.

API par usage :

| Besoin | Fonction | Remarque |
|---|---|---|
| Peindre vite | `span_for_write`, `span_for_read` | Résout la tuile une fois par segment horizontal. Chemin chaud. |
| Signaler une modification | `mark_modified` | Une fois par opération, jamais par pixel. |
| Pixel isolé | `read_pixel`, `write_pixel` | Pratique mais lent : quatre divisions par appel. |
| Lire une zone | `snapshot_region` | Écrit dans un tampon fourni, sans allocation. |
| Copie complète | `snapshot_rgba` | Alloue le document entier. À éviter en boucle. |
| Upload GPU | `dirty_tiles`, `tile_pixels`, `clear_dirty` | Tuiles modifiées depuis le dernier upload. |
| Mémoire | `evict_cold`, `bytes_resident` | Éviction LRU. |
| Annulation | `begin_record`, `commit_record`, `apply_step` | Voir ci-dessous. |

**Éviction** : déclenchée dans `tile_for_write` au moment d'allouer une tuile
neuve, si le total dépasse le budget. Les tuiles les moins récemment touchées
(horodatage `stamp`) partent en premier.

**Annulation par tuiles** : entre `begin_record()` et `commit_record()`, la
première écriture dans chaque tuile en copie l'état précédent. Un pas
d'annulation ne pèse donc que les tuiles réellement touchées. `apply_step`
*échange* le contenu courant et la copie : le pas devient son propre inverse,
ce qui fournit le rétablissement sans stockage supplémentaire. Une tuile qui
n'existait pas est enregistrée vide, pour qu'annuler la fasse disparaître.

Format des pixels : 4 octets **RGBA non prémultiplié**, R en premier.

### 5.3 BrushEngine — `engine/brush/`

- `dab(x, y, rayon, rgba, dureté)` : une empreinte circulaire. `rgba == 0`
  signifie gomme. Parcourt le disque ligne par ligne en calculant sa
  demi-largeur, puis écrit par segments via `span_for_write`. Composition
  « over » : une empreinte ne rend jamais la peinture existante plus
  transparente.
- `segment(de, à, …)` : empreintes interpolées linéairement entre deux
  échantillons.
- `stroke(échantillons)` : lissage Catmull-Rom puis tamponnage avec la brosse
  courante (`IBrush`). Utilisé par `BrushTool` du système d'outils ; l'UI GTK
  n'y passe pas.

Le moteur ne gère ni l'annulation ni les threads.

### 5.4 Calques — `engine/layers/`

**Attention** : l'interface GTK n'utilise pas ce modèle. Ses calques sont
simplement un `std::vector<std::unique_ptr<TileStore>>` dans `main.cpp`, avec
noms, opacités, visibilités et modes dans des tableaux parallèles (`lnames`,
`lop`, `lvis`, `lmode`).

`LayerStack` / `RasterLayer` / `GroupLayer` / `AdjustmentLayer` forment un
modèle générique, destiné aux outils et aux plugins. `LayerCompositor::compose`
les compose sur CPU. Le masque d'un `RasterLayer` n'est créé qu'à la première
demande (`mask()`).

Unifier les deux représentations est un chantier à prévoir (§9).

### 5.5 NativeRenderer — `engine/render/`

Présente un `TileStore` avec GLES2 : texture allouée une seule fois, puis
upload des seules tuiles modifiées (`glTexSubImage2D`), puis un quad plein
écran dans le framebuffer par défaut. Pas de mipmaps. Les tuiles de bord sont
compactées avant l'upload (GLES2 n'a pas `GL_UNPACK_ROW_LENGTH`).

### 5.6 Sérialisation — `engine/serialization/`

Format `.atlas` binaire : en-tête `ATLS`, puis métadonnées (nom, auteur,
version, dimensions, DPI, fond, projection), puis pour chaque calque ses
propriétés et ses pixels et son masque compressés avec zlib. Export PNG et
TIFF de l'image composée.

### 5.7 Outils, fusions, plugins

- `tools/tool_system.*` : interface `ITool`, registre `ToolRegistry`, et
  `ToolCreator` qui valide et exécute des outils écrits en Python (d'où la
  dépendance `python3-embed`). C'est la base des « créateurs d'outils » ;
  l'interface GTK ne s'en sert pas encore.
- `fusion/` : catalogue des modes de fusion avec leur fragment GLSL, et
  `compile_gpu` qui les compile pour les valider au démarrage du GPU.
- Les 16 modes de fusion effectivement utilisés à l'affichage sont
  implémentés en CPU dans `main.cpp` (`blend_px`).

### 5.8 Interface : thème et roue chromatique

**Thème « Observatoire ».** Toute la feuille de style est la constante `CSS`
de `main.cpp` (et une version courte pour l'écran d'accueil dans `activate`).
Palette, à respecter pour tout nouvel élément :

| Rôle | Couleur |
|---|---|
| Fond | `#070b16` |
| Panneaux | `#0a1020` |
| Lignes, bordures | `#16233d` |
| Texte / texte atténué | `#d3dbea` / `#62739a` |
| Actif, sélection (cyan) | `#56c8dc` |
| Marque, titres de section (or) | `#d9b86c` |

Les couleurs restent peu saturées autour du canvas pour ne pas fausser la
perception des couleurs peintes. Le fond autour du document (dégradé,
graticule, étoiles à graine fixe) est rendu une fois par taille de zone dans
`ensure_sky`, puis simplement recopié ; les repères d'angle dorés viennent de
`draw_plate_ticks`.

**Roue chromatique** — `ui/gtk/color_wheel.*`. Teinte sur l'anneau, saturation
et valeur dans le triangle (sommet du haut : teinte pure, bas gauche : blanc,
bas droite : noir). Le triangle ne tourne pas, pour que la main mémorise les
positions. Un point de coordonnées barycentriques (a, b, c) vaut
`a·teinte + b·blanc + c·noir`, d'où `v = a + b` et `s = a / v`.

- Rendu mis en cache : l'anneau par taille, le triangle par taille et teinte.
- Durée de vie liée au widget : `ColorWheel::create()` alloue, GTK libère à la
  finalisation du widget. Ne jamais faire `delete` soi-même.
- `set_rgba` met la roue à jour sans rappeler `on_change`, et garde la teinte
  quand la couleur est grise ou noire, pour que la roue ne saute pas au rouge.
- **Toute modification de `brush_col` hors de la roue** (pipette, préréglage,
  futur nuancier…) doit être suivie de `sync_color_ui()`, sinon la roue et le
  code hexadécimal affichent une couleur périmée.

## 6. Rendu GPU (opt-in)

Activé par `CREATIVE_ENABLE_GPU=1`. Au « realize » du canvas,
`NativeCanvasSurface::initialize()` crée une sous-surface Wayland accrochée à
la fenêtre GTK, un contexte EGL/GLES2 et un `NativeRenderer`. En cas d'échec à
n'importe quelle étape, Atlas reste en CPU et la barre d'état affiche
« Mode CPU (GPU indisponible) ». Hors Wayland (X11), l'init échoue toujours.

**Pourquoi ce n'est pas activé par défaut.** En l'état, activer le GPU dégrade
l'affichage :

1. `refresh_view` ne lui passe que le **calque actif**, pas la composition.
2. `NativeRenderer` dessine un quad plein écran : le document est **étiré** à
   la taille de la zone, sans marge (`GUTTER`) ni respect de l'échelle.
3. La sous-surface est placée **au-dessus** de la surface GTK
   (`wl_subsurface_place_above`) : tout ce que `draw_cb` dessine en Cairo
   — damier, curseur de pinceau, sélection, bordure — est masqué.

Le correctif propre n'est pas de rafistoler la sous-surface mais de passer à
`GtkGLArea` : le rendu GL s'intègre alors au graphe de scène de GTK, et les
éléments d'interface peuvent être dessinés par-dessus normalement (par exemple
dans un `GtkOverlay`). Le travail consiste à :

- envoyer la composition (`cbuf`) plutôt que le calque actif, ou composer les
  calques sur le GPU avec les shaders de fusion déjà écrits dans `fusion/` ;
- appliquer la transformation d'affichage (origine, échelle) dans le vertex
  shader au lieu du quad plein écran ;
- dessiner le damier dans le fragment shader ;
- garder curseur et sélection en Cairo, au-dessus.

## 7. Tablette

Ce que la XP-Pen expose, d'après `atlas_xppen_events.txt` (capture
`libinput debug-events`, à la racine du projet) :

| Périphérique libinput | Rôle |
|---|---|
| `XP-Pen Pen` (event24) | Stylet : position, pression, inclinaison X/Y |
| `XP-Pen Eraser` (event26) | Bout gomme, périphérique séparé |
| `XP-Pen Mouse` (event25) | Émulation souris, **sans aucun axe** |

La pression arrive normalisée 0..1. L'inclinaison est rapportée en degrés par
libinput (valeurs relevées : 10, 15, 16…) ; elle vaut −1 en survol, avant
contact. La tablette est inconnue de libwacom.

Dans `raw_tablet_event` :

- la pression n'est lue que si la source est `GDK_SOURCE_PEN` ; le périphérique
  « Mouse » donne donc toujours une pression de 1 ;
- le retour de `gdk_event_get_axis` est testé, pour distinguer « axe absent »
  de « valeur nulle » ;
- l'inclinaison est normalisée en −1..1 quelle que soit l'unité reçue
  (au-delà de 1,5 en valeur absolue, elle est traitée comme des degrés et
  divisée par 60) ;
- la pression passe par un filtre exponentiel (coefficient 0,55) remis à zéro
  à chaque appui, pour qu'un trait n'hérite pas de la pression de fin du
  précédent.

Effets actuels : la pression module le rayon et l'opacité ; l'inclinaison
élargit légèrement l'empreinte mais **ne l'oriente pas encore**. Le bout gomme
n'est pas encore détecté comme tel.

## 8. Points chauds de performance

Ce qui a été traité, pour ne pas le réintroduire :

| Problème | Solution en place |
|---|---|
| Copie du document entier à chaque frame | `snapshot_region` dans un tampon réutilisé (`snap_buf`) |
| Recomposition de tout le canvas | Dirty-rect alimenté par `dirty_expand` |
| Coût proportionnel au diamètre du pinceau | `span_for_write`, demi-largeur du disque par ligne |
| `revision_` incrémenté par pixel | `mark_modified` une fois par opération |
| Annulation = copie complète du document | Pré-images des seules tuiles touchées |
| Texture GPU réallouée + mipmaps chaque frame | Allocation unique, upload des tuiles modifiées |

Ce qui reste coûteux :

- **`draw_cb`** enveloppe `cbuf` (taille document) dans une surface Cairo et la
  redimensionne vers la taille de la fenêtre à **chaque** frame. GTK4 n'a plus
  de redraw partiel, donc le dirty-rect n'aide pas ici. C'est le principal coût
  restant sur les très grands documents. Correctif : un cache à la résolution
  d'affichage, mis à jour seulement sur la zone modifiée — ou le passage à
  `GtkGLArea` (§6), qui règle les deux problèmes à la fois.
- **`rebuild_bg_cache`** appelle `snapshot_rgba` (allocation complète) pour
  chaque calque. Acceptable parce que rare, mais coûteux à la volée.
- **Déplacer** (`do_move_apply`) et **enregistrer** copient le document entier.
- **Calques au-dessus de l'actif** : recomposés à chaque frame sur le rectangle
  modifié. Peindre sous dix calques coûte environ onze fois plus que peindre au
  sommet. Si cela devient gênant, on peut précomposer les calques du dessus
  quand ils sont tous en mode Normal, cas où le résultat ne dépend pas de ce
  qu'ils recouvrent.

## 9. Problèmes connus

Par ordre de gravité. Corrigé en septembre 2026 : l'ordre des calques pendant
le dessin (le calque actif s'affichait devant les calques du dessus).

1. **Actions non annulables** : déplacement, effacement de calque, suppression
   de calque. Seuls le pinceau, la gomme et le remplissage créent un pas
   d'annulation. Pour en ajouter : `undo_begin()` avant, `undo_commit()` après.
2. **Chemin des icônes codé en dur** : `ICON_PATH` pointe sur
   `/home/deanos/Documents/CreativeSysteme v1.0/UI/icons/`. Atlas n'affichera pas
   ses icônes sur une autre machine ni si ce dossier est déplacé. À remplacer
   par un chemin relatif à l'exécutable ou par des ressources GResource.
3. **Sauvegarde** : chaque calque est enregistré avec un masque entièrement
   opaque de la taille du document. Compressé par zlib donc léger sur disque,
   mais alloué en entier en mémoire à chaque sauvegarde.
4. **Outils non implémentés** : Dégradé, Texte, Doigt, Flou.
5. **Deux modèles de calques** (§5.4) : l'UI et le moteur ne partagent pas la
   même représentation, ce qui bloque le système d'outils et les plugins.

## 10. Reprendre le code

**Ajouter un outil.** Ajouter une valeur à `enum class Tool` dans `main.cpp`,
un bouton dans la barre d'outils, une entrée de menu et une action. Gérer
l'appui dans `drag_begin`, le mouvement dans `drag_update` et le relâchement
dans `drag_end`. Si l'outil modifie des pixels : `undo_begin()` au début,
`dirty_expand()` sur chaque zone touchée, `mark_modified()` sur le store,
`undo_commit()` à la fin, puis `refresh_view()`. Pour les outils de type
pinceau, le chemin passe plutôt par `raw_tablet_event`.

**Écrire dans un calque.** Toujours préférer `span_for_write` à `write_pixel`
dans une boucle. Appeler `mark_modified()` une fois à la fin de l'opération :
c'est la révision qui déclenche la recomposition.

**Modifier un calque inactif** : appeler `invalidate_bg_cache()`.

**Tester la pagination** : panneau mémoire, bouton « Vider la RAM vers le
scratch maintenant », puis dessiner sur l'image existante. Elle doit être
intacte.

**Déboguer la tablette** : `sudo libinput debug-events --show-keycodes` montre
ce que le matériel envoie avant GTK. Si un axe y apparaît mais pas dans Atlas,
le problème est dans `raw_tablet_event`.

**Compiler proprement** : après toute modification de `CMakeLists.txt`,
`rm -rf build` avant de reconfigurer.
