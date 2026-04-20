```

Functionality

When pressed in edit mode, a snapshot of the current button order is taken.

During dragging, as soon as your finger hovers over another cell, the neighboring cells slide (180ms animation ease_out) to fill the space—just like on iOS.

When released, the selected button lands on its logical cell; the others are already in their new positions.


Option (a) — draggable: false: each button input now accepts a dict form. A pinned button retains its native YAML (on_press, on_long_press, on_click...) — no overlay, no relay. It occupies a cell that the reflow cannot access.

buttons:
  - btn_light                  # draggable (forme courte)
  - id: btn_alarm              # draggable (forme dict)
  - id: btn_settings           # pinne
    draggable: false

3) Option (b) — external trigger: two YAML actions to enter/exit edit mode from any button (including a pinned button):

on_long_press:
  - draggable_grid.toggle_edit_mode: my_grid
  # ou
  - draggable_grid.set_edit_mode:
      id: my_grid
      value: true

Direct use case for your light button: set `draggable: false` on it → `on_press` toggles the light, `on_long_press` opens the RGB modal, no conflict with edit mode. Edit mode is activated from another button via `draggable_grid.toggle_edit_mode`.



external_components:
  - source:
      type: local
      path: components
    components: [draggable_grid]


# ----------------------------------------------------------------------------
# the draggable_grid declaration (anywhere at the root level)
# ----------------------------------------------------------------------------
# Order of boxes = order of buttons.
# cell_w / cell_h are the dimensions of a button (to detect the nearest box on drop).

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
    
