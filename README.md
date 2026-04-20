```

fonctionnement

Au PRESSED en edit mode, snapshot de l'ordre actuel des boutons.
Pendant le drag, dès que ton doigt survole une autre case, les voisins coulissent (anim 180 ms ease_out) pour combler — comme sur iOS.
Au lâcher, le bouton saisi se pose sur la case qu'il occupe logiquement, les autres sont déjà à leur nouvelle place.


Option (a) — draggable: false : chaque entrée de buttons accepte désormais une forme dict. Un bouton pinné garde son YAML natif (on_press, on_long_press, on_click...) — aucun overlay, aucun relais. Il occupe une case que le reflow ne peut pas toucher :

buttons:
  - btn_light                  # draggable (forme courte)
  - id: btn_alarm              # draggable (forme dict)
  - id: btn_settings           # pinne
    draggable: false

3) Option (b) — trigger externe : deux actions YAML pour entrer/sortir du edit mode depuis n'importe quel bouton (y compris un bouton pinné) :

on_long_press:
  - draggable_grid.toggle_edit_mode: my_grid
  # ou
  - draggable_grid.set_edit_mode:
      id: my_grid
      value: true

Cas d'usage direct pour ton bouton lumière : mets draggable: false dessus → on_press toggle la lumière, on_long_press ouvre la modale RGB, aucun conflit avec le mode édition. Le mode édition s'active depuis un autre bouton via draggable_grid.toggle_edit_mode



external_components:
  - source:
      type: local
      path: components
    components: [draggable_grid]


# ----------------------------------------------------------------------------
# BLOC 2 — la declaration draggable_grid (n'importe ou au niveau racine)
# ----------------------------------------------------------------------------
# Ordre des cases = ordre des boutons.
# cell_w / cell_h sont les dimensions d'un bouton (pour detecter la case la
# plus proche au drop).
draggable_grid:
  cell_w: 150
  cell_h: 100
  cells:
    - [10, 220]     # case 0 : col 0, row 0
    - [200, 220]    # case 1 : col 1, row 0
    - [380, 220]    # case 2 : col 2, row 0
    - [10, 345]     # case 3 : col 0, row 1
    - [200, 345]    # case 4 : col 1, row 1
    - [380, 345]    # case 5 : col 2, row 1
    - [10, 480]     # case 6 : col 0, row 2
    - [200, 480]    # case 7 : col 1, row 2
    - [380, 480]    # case 8 : col 2, row 2
  buttons:
    - btn_light
    - btn_alarm
    - btn_test3
    - btn_player
    - btn_camera
    - btn_test4
    - btn_settings
    - btn_test
    - btn_test2
```
    
