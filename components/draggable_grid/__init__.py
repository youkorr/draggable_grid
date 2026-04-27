"""ESPHome LVGL draggable grid component.

100% declaratif : l'utilisateur declare une liste de cases (x, y) et
une liste de widgets LVGL (n'importe quel obj : button, image, panneau
custom, etc.) ; le composant genere tout le cablage C++ au build.
Aucun lambda requis cote YAML.

Toutes les animations et le comportement par-bouton sont configurables :
voir README.md.

Exemple minimal :

    external_components:
      - source: components

    draggable_grid:
      id: my_grid
      cell_w: 150
      cell_h: 100
      cells:
        - [10, 220]
        - [200, 220]
      buttons:
        - btn_light
        - id: btn_settings
          draggable: false
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.const import CONF_ID, CONF_VALUE

# On accepte n'importe quel widget LVGL (button, obj, image, panel custom...).
# `lv_pseudo_button_t` est trop restrictif : il refuse les `obj` ou `image`
# que des utilisateurs publics peuvent vouloir mettre dans la grille.
try:
    from esphome.components.lvgl.types import lv_obj_base_t as _LV_BASE
except ImportError:  # fallback pour les forks ou les versions plus anciennes
    from esphome.components.lvgl.types import lv_pseudo_button_t as _LV_BASE

CODEOWNERS = ["@youkorr"]
DEPENDENCIES = ["lvgl"]

draggable_grid_ns = cg.esphome_ns.namespace("draggable_grid_cmpt")
DraggableGridComponent = draggable_grid_ns.class_(
    "DraggableGridComponent", cg.Component
)
ToggleEditModeAction = draggable_grid_ns.class_(
    "ToggleEditModeAction", automation.Action
)
SetEditModeAction = draggable_grid_ns.class_(
    "SetEditModeAction", automation.Action
)

# --- Cles de config ------------------------------------------------
CONF_CELL_W = "cell_w"
CONF_CELL_H = "cell_h"
CONF_CELLS = "cells"
CONF_BUTTONS = "buttons"
CONF_DRAGGABLE = "draggable"

# Globales (grille)
CONF_REFLOW_MS = "reflow_ms"
CONF_BREATHE_AMPLITUDE = "breathe_amplitude"
CONF_BREATHE_PERIOD_MS = "breathe_period_ms"
CONF_BREATHE_PHASE_STEP_MS = "breathe_phase_step_ms"
CONF_LONG_PRESS_TO_EDIT = "long_press_to_edit"

# Defauts par-bouton (au niveau grille, applique a tous les boutons
# qui ne redefinissent pas l'option)
CONF_BREATHE = "breathe"
CONF_RELAY_PRESS_IMMEDIATE = "relay_press_immediate"
CONF_APPLY_PRESSED_STATE = "apply_pressed_state"
CONF_LIFT_Y = "lift_y"


def _cell(value):
    """Validate a single cell entry [x, y]."""
    if not isinstance(value, list) or len(value) != 2:
        raise cv.Invalid("Each cell must be a 2-element list [x, y]")
    return [cv.int_(v) for v in value]


# Toutes les options par-bouton sont optionnelles SANS default ici :
# le default est resolu en `to_code` depuis le niveau grille (qui
# lui-meme tombe sur des constantes si l'utilisateur n'a rien mis).
_BUTTON_DICT_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.use_id(_LV_BASE),
        cv.Optional(CONF_DRAGGABLE, default=True): cv.boolean,
        cv.Optional(CONF_BREATHE): cv.boolean,
        cv.Optional(CONF_RELAY_PRESS_IMMEDIATE): cv.boolean,
        cv.Optional(CONF_APPLY_PRESSED_STATE): cv.boolean,
        cv.Optional(CONF_LIFT_Y): cv.int_,
    }
)


def _button_entry(value):
    """Accepte :
    - une id LVGL nue (toutes options par defaut)
    - un dict {id: <btn_id>, draggable: ..., breathe: ..., ...}
    """
    if isinstance(value, dict):
        return _BUTTON_DICT_SCHEMA(value)
    return _BUTTON_DICT_SCHEMA({CONF_ID: value})


# --- Defauts effectifs si rien n'est specifie cote YAML ------------
_DEFAULT_REFLOW_MS = 150
_DEFAULT_BREATHE_AMPLITUDE = -2
_DEFAULT_BREATHE_PERIOD_MS = 1000
_DEFAULT_BREATHE_PHASE_STEP_MS = 100
_DEFAULT_LONG_PRESS_TO_EDIT = True
_DEFAULT_BREATHE = True
_DEFAULT_RELAY_PRESS_IMMEDIATE = True
_DEFAULT_APPLY_PRESSED_STATE = True
_DEFAULT_LIFT_Y = -6


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(DraggableGridComponent),
        cv.Optional(CONF_CELL_W, default=150): cv.positive_int,
        cv.Optional(CONF_CELL_H, default=100): cv.positive_int,
        cv.Required(CONF_CELLS): cv.ensure_list(_cell),
        cv.Required(CONF_BUTTONS): cv.ensure_list(_button_entry),

        # --- Globales d'animation (grille) -------------------------
        cv.Optional(CONF_REFLOW_MS, default=_DEFAULT_REFLOW_MS):
            cv.positive_int,
        cv.Optional(CONF_BREATHE_AMPLITUDE, default=_DEFAULT_BREATHE_AMPLITUDE):
            cv.int_,
        cv.Optional(CONF_BREATHE_PERIOD_MS, default=_DEFAULT_BREATHE_PERIOD_MS):
            cv.positive_int,
        cv.Optional(CONF_BREATHE_PHASE_STEP_MS,
                    default=_DEFAULT_BREATHE_PHASE_STEP_MS):
            cv.positive_int,
        cv.Optional(CONF_LONG_PRESS_TO_EDIT,
                    default=_DEFAULT_LONG_PRESS_TO_EDIT):
            cv.boolean,

        # --- Defauts par-bouton (grille) ---------------------------
        # Surchargeable individuellement dans `buttons:`.
        cv.Optional(CONF_BREATHE, default=_DEFAULT_BREATHE): cv.boolean,
        cv.Optional(CONF_RELAY_PRESS_IMMEDIATE,
                    default=_DEFAULT_RELAY_PRESS_IMMEDIATE):
            cv.boolean,
        cv.Optional(CONF_APPLY_PRESSED_STATE,
                    default=_DEFAULT_APPLY_PRESSED_STATE):
            cv.boolean,
        cv.Optional(CONF_LIFT_Y, default=_DEFAULT_LIFT_Y): cv.int_,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    cells = config[CONF_CELLS]
    buttons = config[CONF_BUTTONS]
    if len(cells) != len(buttons):
        raise cv.Invalid(
            f"'cells' ({len(cells)}) et 'buttons' ({len(buttons)}) "
            "doivent avoir la meme longueur"
        )

    cg.add_global(
        cg.RawStatement(
            '#include "esphome/components/draggable_grid/draggable_grid.h"'
        )
    )

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # --- Globales grille ------------------------------------------
    cg.add(var.set_cell_size(config[CONF_CELL_W], config[CONF_CELL_H]))
    cg.add(var.set_reflow_ms(config[CONF_REFLOW_MS]))
    cg.add(var.set_breathe_amplitude(config[CONF_BREATHE_AMPLITUDE]))
    cg.add(var.set_breathe_period_ms(config[CONF_BREATHE_PERIOD_MS]))
    cg.add(var.set_breathe_phase_step_ms(config[CONF_BREATHE_PHASE_STEP_MS]))
    cg.add(var.set_long_press_to_edit(config[CONF_LONG_PRESS_TO_EDIT]))

    # --- Boutons (avec resolution per-button vs grille) -----------
    grid_breathe = config[CONF_BREATHE]
    grid_relay   = config[CONF_RELAY_PRESS_IMMEDIATE]
    grid_apply   = config[CONF_APPLY_PRESSED_STATE]
    grid_lift_y  = config[CONF_LIFT_Y]

    for idx, entry in enumerate(buttons):
        cx, cy = cells[idx]
        btn = await cg.get_variable(entry[CONF_ID])

        draggable = entry[CONF_DRAGGABLE]
        breathe   = entry.get(CONF_BREATHE, grid_breathe)
        relay     = entry.get(CONF_RELAY_PRESS_IMMEDIATE, grid_relay)
        apply_st  = entry.get(CONF_APPLY_PRESSED_STATE, grid_apply)
        lift_y    = entry.get(CONF_LIFT_Y, grid_lift_y)

        cg.add(var.add(btn, idx, cx, cy,
                       draggable, breathe, relay, apply_st, lift_y))


# ---------------------------------------------------------------
# Actions : permettent d'entrer / sortir du edit mode depuis
# n'importe quel trigger YAML (on_press, on_long_press, etc.).
# ---------------------------------------------------------------
_ACTION_BASE_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(DraggableGridComponent),
    }
)


@automation.register_action(
    "draggable_grid.toggle_edit_mode",
    ToggleEditModeAction,
    _ACTION_BASE_SCHEMA,
)
async def toggle_edit_mode_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action(
    "draggable_grid.set_edit_mode",
    SetEditModeAction,
    _ACTION_BASE_SCHEMA.extend(
        {
            cv.Required(CONF_VALUE): cv.templatable(cv.boolean),
        }
    ),
)
async def set_edit_mode_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    template_ = await cg.templatable(config[CONF_VALUE], args, bool)
    cg.add(var.set_value(template_))
    return var
