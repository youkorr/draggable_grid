```
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
    
