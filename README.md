# ESPHome LVGL Draggable Grid

A declarative ESPHome component for arranging LVGL buttons on a draggable grid.

The component is configured entirely from YAML:
- define a list of grid cells,
- define a matching list of LVGL buttons,
- the component generates the C++ wiring automatically.

No YAML lambdas are required.

---

## Features

- **Declarative YAML configuration**
- **Drag and reorder buttons in edit mode**
- **iOS-style live reflow** while dragging
- **Pinned buttons** with `draggable: false`
- **External actions** to toggle or set edit mode from any trigger
- Stable button identity during reordering
- No user-side lambdas

---

## Installation

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [draggable_grid]
```

---

## Requirements

- ESPHome with **LVGL** enabled
- LVGL button widgets already declared in YAML
- The number of `cells` must match the number of `buttons`

---

## Basic configuration

`cell_w` and `cell_h` are used to determine the nearest target cell during dragging.

```yaml
draggable_grid:
  id: my_grid
  cell_w: 150
  cell_h: 100
  cells:
    - [10, 220]
    - [200, 220]
    - [380, 220]
    - [10, 345]
    - [200, 345]
    - [380, 345]
    - [10, 480]
    - [200, 480]
    - [380, 480]
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

---

## Button entry formats

Each entry in `buttons:` supports two formats.

### Short form

By default, the button is draggable.

```yaml
buttons:
  - btn_light
  - btn_alarm
```

### Dictionary form

Use this form to explicitly configure `draggable`.

```yaml
buttons:
  - id: btn_light
    draggable: true
  - id: btn_settings
    draggable: false
```

---

## Behavior

### Normal mode

- **Single click**: ignored
- **Double click**: relayed to the original button, so its native button action is triggered
- **Long press**: enters edit mode

### Edit mode

- Draggable buttons animate gently to indicate edit mode
- The selected button follows the finger during drag
- Neighboring buttons **reflow live** while dragging
- On release, the dragged button snaps to the nearest valid cell
- Long press without dragging exits edit mode

---

## Pinned buttons

Set `draggable: false` to make a button pinned.

Pinned buttons:

- keep their native YAML/LVGL behavior,
- do **not** get an overlay,
- are **not draggable**,
- keep their own `on_press`, `on_long_press`, `on_click`, etc.,
- occupy a cell that reflow cannot use.

Example:

```yaml
draggable_grid:
  id: my_grid
  cell_w: 150
  cell_h: 100
  cells:
    - [10, 220]
    - [200, 220]
    - [380, 220]
  buttons:
    - btn_light
    - id: btn_alarm
    - id: btn_settings
      draggable: false
```

Practical use case:

- a pinned light button keeps:
  - `on_press` -> toggle light
  - `on_long_press` -> open RGB modal
- edit mode is triggered from another button

---

## External edit mode actions

Edit mode can be controlled from any YAML trigger.

### Toggle edit mode

```yaml
on_long_press:
  - draggable_grid.toggle_edit_mode: my_grid
```

### Set edit mode explicitly

```yaml
on_long_press:
  - draggable_grid.set_edit_mode:
      id: my_grid
      value: true
```

### Disable edit mode explicitly

```yaml
on_press:
  - draggable_grid.set_edit_mode:
      id: my_grid
      value: false
```

This is useful when:

- the trigger button itself is pinned,
- edit mode should be controlled from another widget,
- you want a dedicated **Edit** button.

---

## Example: pinned button + external trigger

```yaml
draggable_grid:
  id: my_grid
  cell_w: 150
  cell_h: 100
  cells:
    - [10, 220]
    - [200, 220]
    - [380, 220]
  buttons:
    - id: btn_light
      draggable: false
    - btn_alarm
    - btn_settings

lvgl:
  widgets:
    - button:
        id: btn_edit_trigger
        on_long_press:
          - draggable_grid.toggle_edit_mode: my_grid
```

In this setup:

- `btn_light` keeps its native behavior
- `btn_alarm` and `btn_settings` remain draggable
- `btn_edit_trigger` toggles edit mode externally

---

## How reordering works

When a drag starts in edit mode:

1. the current button order is snapshotted,
2. the dragged button follows the finger,
3. when it moves closer to another cell, neighboring buttons slide to fill the gap,
4. on release, the dragged button animates into its final logical cell.

Pinned buttons remain fixed and are excluded from reflow.

---

## Notes

- `cells` and `buttons` must have the same length
- Maximum supported button count is currently **16**
- Pinned buttons keep their original event handling
- Draggable buttons use an overlay for interaction routing
- The component depends on the ESPHome `lvgl` integration

---

## Summary

This component provides a fully declarative way to build a draggable LVGL grid in ESPHome, with:

- live reordering,
- pinned cells,
- external edit-mode control,
- and no YAML lambdas required.
    
