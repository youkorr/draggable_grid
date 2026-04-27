#pragma once
//
// Native ESPHome component for LVGL draggable grid (Option A, iOS-style).
// 100% declaratif cote YAML : aucune lambda ne doit etre ecrite par
// l'utilisateur, et le YAML des boutons reste inchange.
//
// Gestuelle (v5) :
//   - simple clic sur un bouton      -> l'event LV_EVENT_PRESSED est
//                                       relaye au bouton -> on_press se
//                                       declenche (ouvre la page).
//   - long-press sur un bouton       -> entre en "edit mode".
//     (en edit mode)
//   - drag d'un bouton               -> SWAP : le bouton survole echange
//                                       sa cellule avec le bouton drague.
//                                       Aucun autre bouton ne bouge (le
//                                       shift lineaire iOS-style donnait
//                                       des deplacements parasites en
//                                       drag vertical sur une grille
//                                       multi-colonnes).
//                                       Au lacher, le bouton se pose sur
//                                       la derniere case survolee.
//   - long-press sans bouger         -> sortie du edit mode.
//
// Feedback visuel (tout base sur la translation = PPA-friendly sur
// ESP32-P4 ; transform_scale est volontairement evite car l'unite PPA
// rejette les blits scales et bascule en software rendering) :
//   - edit mode : tous les boutons "respirent" par bob translate_y de
//     0 -> -2 px (1000 ms A/R, dephasage par idx). Le bouton saisi
//     est souleve de -6 px (translate_y) pendant le drag.
//   - normal mode : LV_STATE_PRESSED est manuellement applique au
//     bouton sur press/release pour conserver le style `pressed:`
//     defini en YAML (translate_y, bg_color, etc.).
//
// Architecture
// ------------
//   Button (toujours NON-CLICKABLE : ne recoit plus d'event directement)
//     +-- Overlay (enfant, TOUJOURS visible et clickable)
//         Unique point d'entree pour toutes les interactions. Route
//         soit vers simple-click-relay, soit vers drag-edit selon
//         g_edit_mode.
//
// Etat
// ----
// - Stockage statique (inline variables) : ~300 octets, zero heap.
// - g_active / g_moved sont remis a 0/-1 en fin de chaque press.
// - L'identite d'un bouton (idx) est STABLE : g_buttons[idx] et
//   g_overlays[idx] ne sont jamais reordonnes. Les swaps ne touchent
//   que g_cell_of[] et g_button_at[], donc le user_data du callback
//   reste valide apres un nombre quelconque d'echanges.
//

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "lvgl.h"
#include <cstdint>
#include <climits>

// ============================================================
// Engine (pur LVGL)
// ============================================================
namespace draggable_grid {

constexpr int MAX_BUTTONS = 16;

struct Cell { int16_t x; int16_t y; };

// Geometrie configurable (mise a jour par le Component ESPHome au setup)
inline int16_t g_cell_w = 150;
inline int16_t g_cell_h = 100;

// --- Globaux configurables (toute la grille) ----------------------
// Reflow : duree d'anim de remise en place des voisins lors d'un swap.
inline uint32_t g_reflow_ms = 150;
// Respiration en edit mode : amplitude (px), periode (ms), dephasage par idx (ms).
inline int16_t  g_breathe_amplitude = -2;
inline uint32_t g_breathe_period_ms = 1000;
inline uint32_t g_breathe_phase_step_ms = 100;
// Long-press sur tuile pour entrer en edit mode (false => seul l'action
// externe `draggable_grid.toggle_edit_mode` peut basculer).
inline bool g_long_press_to_edit = true;

// Tables FIXES, indexees par identite de bouton (idx).
inline lv_obj_t* g_buttons[MAX_BUTTONS]{};   // pointeur du bouton idx
inline lv_obj_t* g_overlays[MAX_BUTTONS]{};  // pointeur de l'overlay idx
inline Cell      g_cells[MAX_BUTTONS]{};     // position d'une cellule
inline bool      g_pinned[MAX_BUTTONS]{};    // draggable=false : pas de
                                             // overlay, pas dans le reflow

// --- Overrides par-bouton (resolus au YAML, defaut = global) ------
inline bool     g_breathe[MAX_BUTTONS]{};                // anim respiration en edit mode
inline bool     g_relay_press_immediate[MAX_BUTTONS]{};  // PRESSED relay au touch
inline bool     g_apply_pressed_state[MAX_BUTTONS]{};    // ajout LV_STATE_PRESSED
inline int16_t  g_lift_y[MAX_BUTTONS]{};                 // translate_y pendant le drag

// Mappings courants cellule <-> bouton. Mis a jour uniquement au swap.
inline int8_t    g_cell_of[MAX_BUTTONS]{};
inline int8_t    g_button_at[MAX_BUTTONS]{};

inline int8_t    g_count = 0;
inline int8_t    g_active = -1;        // -1 = idle, sinon idx de bouton
inline bool      g_moved = false;      // drag en cours si true
inline bool      g_long_fired = false; // le press en cours a deja tire
                                       // LONG_PRESSED -> ignorer le
                                       // relay simple-click au RELEASED
inline bool      g_edit_mode = false;

// Parent scroll lock pendant le drag : l'indev LVGL peut sinon
// convertir la gestuelle en scroll sur un ancetre scrollable.
inline lv_obj_t* g_drag_parent = nullptr;
inline bool      g_parent_was_scrollable = false;

// Cellule actuellement occupee par le bouton saisi (mise a jour pendant
// le PRESSING). Sert a eviter de re-declencher un swap quand le doigt
// reste dans la meme case.
inline int8_t    g_last_target_cell = -1;

// --- helpers internes ---------------------------------------
// Renvoie -1 si aucune cellule libre (tout pinned, improbable).
inline int8_t nearest_cell(int32_t cx, int32_t cy) {
  int32_t best = INT32_MAX;
  int8_t  best_i = -1;
  for (int8_t i = 0; i < g_count; ++i) {
    // Une cellule occupee par un bouton pinne est intangible : le
    // bouton drague ne peut pas s'y poser ni pousser son occupant.
    const int8_t occupant = g_button_at[i];
    if (occupant >= 0 && g_pinned[occupant]) continue;
    int32_t dx = cx - (g_cells[i].x + g_cell_w / 2);
    int32_t dy = cy - (g_cells[i].y + g_cell_h / 2);
    int32_t d  = dx * dx + dy * dy;
    if (d < best) { best = d; best_i = i; }
  }
  return best_i;
}

// Animation de position packee (x+y dans un seul lv_anim_t, 1 call
// lv_obj_set_pos par frame au lieu de 2). Cible : ESP32-P4 sans CPU
// burn inutile.
struct MoveAnim {
  lv_obj_t* btn;
  int16_t start_x, start_y;
  int16_t end_x,   end_y;
};
inline MoveAnim g_move_anims[MAX_BUTTONS]{};

inline void anim_pos_exec_cb(void* var, int32_t v) {
  MoveAnim* m = static_cast<MoveAnim*>(var);
  if (m == nullptr || m->btn == nullptr) return;
  int32_t x = m->start_x + ((m->end_x - m->start_x) * v) / 1000;
  int32_t y = m->start_y + ((m->end_y - m->start_y) * v) / 1000;
  lv_obj_set_pos(m->btn, x, y);
}

inline void animate_btn_to(int8_t btn_idx, int32_t dst_x, int32_t dst_y) {
  if (btn_idx < 0 || btn_idx >= MAX_BUTTONS) return;
  lv_obj_t* btn = g_buttons[btn_idx];
  if (btn == nullptr) return;

  MoveAnim& m = g_move_anims[btn_idx];
  m.btn     = btn;
  m.start_x = static_cast<int16_t>(lv_obj_get_x_aligned(btn));
  m.start_y = static_cast<int16_t>(lv_obj_get_y_aligned(btn));
  m.end_x   = static_cast<int16_t>(dst_x);
  m.end_y   = static_cast<int16_t>(dst_y);

  if (m.start_x == m.end_x && m.start_y == m.end_y) return;

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, &m);
  lv_anim_set_exec_cb(&a, anim_pos_exec_cb);
  lv_anim_set_time(&a, g_reflow_ms);
  lv_anim_set_values(&a, 0, 1000);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

inline void kill_pos_anim(int8_t btn_idx) {
  if (btn_idx < 0 || btn_idx >= MAX_BUTTONS) return;
  lv_anim_delete(&g_move_anims[btn_idx], anim_pos_exec_cb);
}

// Swap pur : le bouton drague echange sa cellule avec l'occupant de
// target_cell. Aucun autre bouton ne bouge.
//
// Pourquoi pas un shift lineaire iOS-style ? Le shift parcourt les
// cellules dans l'ordre d'index : pour un drag horizontal sur une
// grille multi-colonnes, les cellules adjacentes en index sont aussi
// adjacentes a l'ecran, donc le shift ressemble a un swap. Mais pour
// un drag vertical, target_cell est plusieurs index plus loin que
// la cellule source, et toutes les cellules intermediaires (donc des
// boutons sur d'autres lignes/colonnes) glissent inutilement.
//
// Si target_cell est occupee par un bouton pinne, on l'ignore : le
// bouton pinne ne peut pas etre deplace, donc on n'echange rien.
inline void reflow_to(int8_t dragged_idx, int8_t target_cell) {
  if (target_cell == g_last_target_cell) return;
  if (target_cell < 0) return;
  g_last_target_cell = target_cell;

  const int8_t src_cell = g_cell_of[dragged_idx];
  if (src_cell == target_cell) return;

  const int8_t target_occupant = g_button_at[target_cell];
  if (target_occupant >= 0 && g_pinned[target_occupant]) return;

  g_button_at[target_cell] = dragged_idx;
  g_cell_of[dragged_idx]   = target_cell;

  if (target_occupant >= 0 && target_occupant != dragged_idx) {
    g_button_at[src_cell]      = target_occupant;
    g_cell_of[target_occupant] = src_cell;
    animate_btn_to(target_occupant,
                   g_cells[src_cell].x, g_cells[src_cell].y);
  } else {
    g_button_at[src_cell] = -1;
  }
}

// Breathing : bob translate_y de +-2 px, 1000 ms A/R, infini.
// On evite volontairement transform_scale : l'unite PPA de l'ESP32-P4
// rejette les blits scales (voir lv_draw_ppa.c ppa_evaluate) et bascule
// en software scaling, tres couteux. Une translation pure reste dans
// le chemin rapide PPA (fill + image blit acceleres).
inline void breathe_exec_cb(void* var, int32_t v) {
  lv_obj_set_style_translate_y(
      static_cast<lv_obj_t*>(var), v, LV_PART_MAIN);
}

inline void start_breathe(lv_obj_t* btn, int8_t idx) {
  if (btn == nullptr) return;

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, btn);
  lv_anim_set_exec_cb(&a, breathe_exec_cb);
  lv_anim_set_time(&a, g_breathe_period_ms);
  lv_anim_set_playback_time(&a, g_breathe_period_ms);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_values(&a, 0, g_breathe_amplitude);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_set_delay(&a, static_cast<uint32_t>(idx) * g_breathe_phase_step_ms);
  lv_anim_start(&a);
}

inline void stop_breathe(lv_obj_t* btn) {
  if (btn == nullptr) return;
  lv_anim_delete(btn, breathe_exec_cb);
  lv_obj_set_style_translate_y(btn, 0, LV_PART_MAIN);
}

// Coupe le breathing sur tous les boutons : appele au debut d'un drag
// pour degager le pipeline PPA (translate_y sur N boutons en parallele
// coute un redraw chacun, pas necessaire pendant un drag).
inline void pause_all_breathe() {
  for (int8_t i = 0; i < g_count; ++i) {
    if (g_pinned[i]) continue;
    if (!g_breathe[i]) continue;
    stop_breathe(g_buttons[i]);
  }
}

// Relance le breathing sur tous les boutons apres un drop si on est
// toujours en edit mode.
inline void resume_all_breathe() {
  for (int8_t i = 0; i < g_count; ++i) {
    if (g_pinned[i]) continue;
    if (!g_breathe[i]) continue;
    start_breathe(g_buttons[i], i);
  }
}

inline void set_edit_mode(bool enabled) {
  g_edit_mode = enabled;
  for (int8_t i = 0; i < g_count; ++i) {
    lv_obj_t* btn = g_buttons[i];
    if (btn == nullptr) continue;
    if (g_pinned[i]) continue;  // les pinned ne respirent pas : pas draggables
    if (!g_breathe[i]) continue;  // breathing desactive pour ce bouton
    if (enabled) {
      start_breathe(btn, i);
    } else {
      stop_breathe(btn);
    }
  }
}

inline void toggle_edit_mode() { set_edit_mode(!g_edit_mode); }

// Callback attache A L'OVERLAY (toujours visible). Unique point
// d'entree pour toutes les interactions utilisateur.
//
// user_data = identite STABLE du bouton (idx d'origine a l'attach).
// Apres un swap, g_buttons[idx] et g_overlays[idx] ne changent PAS ;
// seul g_cell_of[idx] est mis a jour.
inline void overlay_event_cb(lv_event_t* e) {
  const lv_event_code_t code = lv_event_get_code(e);
  const int8_t idx = static_cast<int8_t>(
      reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
  if (idx < 0 || idx >= g_count) return;
  lv_obj_t* btn = g_buttons[idx];
  if (btn == nullptr) return;

  // --- LONG_PRESSED -------------------------------------------------
  // En edit mode : sortie du edit mode (sauf si on est en plein drag).
  // En normal mode : entree en edit mode (si g_long_press_to_edit).
  if (code == LV_EVENT_LONG_PRESSED) {
    if (g_edit_mode && g_active == idx && g_moved) return;  // drag actif
    // Honorer la config : si l'edit mode est externalise (bouton dedie),
    // on laisse passer le long-press sans rien faire ici. La sortie de
    // edit mode reste possible via long-press tile (utile par defaut).
    if (!g_edit_mode && !g_long_press_to_edit) return;
    g_long_fired = true;        // annule le relay simple-click au RELEASED
    g_active = -1;
    g_moved = false;
    if (g_apply_pressed_state[idx]) {
      lv_obj_remove_state(btn, LV_STATE_PRESSED);  // propre cote visuel
    }
    toggle_edit_mode();
    return;
  }

  // --- PRESSED ------------------------------------------------------
  if (code == LV_EVENT_PRESSED) {
    g_active = idx;
    g_moved = false;
    g_long_fired = false;
    if (g_edit_mode) {
      // Coupe TOUTES les anims de respiration pour liberer le pipeline
      // PPA pendant le drag + reflow (gain majeur sur ESP32-P4).
      pause_all_breathe();
      // Lift visuel du bouton saisi : translate_y pure, pas de scale,
      // donc reste dans le chemin rapide PPA. Amplitude configurable
      // par bouton (g_lift_y).
      lv_obj_move_foreground(btn);
      lv_obj_set_style_translate_y(btn, g_lift_y[idx], LV_PART_MAIN);
      // Annule toute anim de position en cours sur les voisins / sur le
      // bouton saisi (qui suit le doigt 1:1).
      for (int8_t i = 0; i < g_count; ++i) kill_pos_anim(i);
      g_last_target_cell = g_cell_of[idx];
      // Bloque le scroll du parent pendant le drag : sinon un mouvement
      // vers le bas fait descendre tout l'affichage (LVGL convertit le
      // geste en scroll sur l'ancetre scrollable).
      g_drag_parent = lv_obj_get_parent(btn);
      if (g_drag_parent != nullptr) {
        g_parent_was_scrollable =
            lv_obj_has_flag(g_drag_parent, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(g_drag_parent, LV_OBJ_FLAG_SCROLLABLE);
      }
    } else {
      // Visuel : applique le style `pressed:` du YAML si le widget en
      // a un (configurable, certains widgets/themes n'en definissent pas).
      if (g_apply_pressed_state[idx]) {
        lv_obj_add_state(btn, LV_STATE_PRESSED);
      }
      // Snappy : si activé, on relaie PRESSED au touch (au lieu de la
      // release) pour que on_press: soit instantane. Sinon comportement
      // historique (relay au RELEASED, voir branche ci-dessous).
      if (g_relay_press_immediate[idx]) {
        lv_obj_send_event(btn, LV_EVENT_PRESSED, nullptr);
      }
    }
    return;
  }

  // --- PRESSING (drag + reflow en edit mode uniquement) -------------
  if (code == LV_EVENT_PRESSING) {
    if (!g_edit_mode) return;
    if (g_active != idx) return;
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_point_t v;
    lv_indev_get_vect(indev, &v);
    if (v.x == 0 && v.y == 0) return;
    g_moved = true;
    // 1) le bouton saisi suit le doigt.
    int32_t nx = lv_obj_get_x_aligned(btn) + v.x;
    int32_t ny = lv_obj_get_y_aligned(btn) + v.y;
    lv_obj_set_pos(btn, nx, ny);
    // 2) reflow eventuel si le bouton saisi est desormais plus proche
    //    d'une autre case.
    int32_t cx = nx + g_cell_w / 2;
    int32_t cy = ny + g_cell_h / 2;
    int8_t  target = nearest_cell(cx, cy);
    if (target != g_last_target_cell) {
      reflow_to(idx, target);
    }
    return;
  }

  // --- RELEASED / PRESS_LOST ----------------------------------------
  if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    if (g_active != idx) return;
    g_active = -1;

    // ---- NORMAL MODE : simple-click relay ---------------------------
    if (!g_edit_mode) {
      if (g_apply_pressed_state[idx]) {
        lv_obj_remove_state(btn, LV_STATE_PRESSED);
      }
      const bool short_click =
          (code == LV_EVENT_RELEASED) && !g_moved && !g_long_fired;
      g_moved = false;
      g_long_fired = false;
      if (short_click) {
        // Si PRESSED n'a PAS ete relaye au touch (mode legacy), on le
        // relaie maintenant. Sinon on a deja envoye PRESSED au touch et
        // on complete simplement la chaine RELEASED + CLICKED.
        if (!g_relay_press_immediate[idx]) {
          lv_obj_send_event(btn, LV_EVENT_PRESSED, nullptr);
        }
        lv_obj_send_event(btn, LV_EVENT_RELEASED, nullptr);
        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
      }
      return;
    }

    // ---- EDIT MODE : fin du drag (reflow deja applique pdt PRESSING) -
    g_moved = false;
    g_long_fired = false;
    g_last_target_cell = -1;

    // Le bouton saisi se pose anime sur la case finale (celle deja
    // inscrite dans g_cell_of par le dernier reflow_to).
    const int8_t dst_cell = g_cell_of[idx];
    animate_btn_to(idx, g_cells[dst_cell].x, g_cells[dst_cell].y);

    // Restaure le scroll du parent si on l'avait desactive.
    if (g_drag_parent != nullptr && g_parent_was_scrollable) {
      lv_obj_add_flag(g_drag_parent, LV_OBJ_FLAG_SCROLLABLE);
    }
    g_drag_parent = nullptr;
    g_parent_was_scrollable = false;

    // Relance le breathing global apres le drop (si on est encore en
    // edit mode, ce qui est le cas par definition ici).
    resume_all_breathe();
  }
}

// Options par-bouton resolues depuis le YAML (defaut = comportement
// historique). Tout est explicite pour qu'un widget custom (panneau,
// image, obj...) puisse etre integre dans la grille sans hypothese
// implicite sur ses styles.
struct AttachOptions {
  bool    draggable             = true;
  bool    breathe               = true;   // anim respiration en edit mode
  bool    relay_press_immediate = true;   // PRESSED relay au touch (snappy)
  bool    apply_pressed_state   = true;   // ajout LV_STATE_PRESSED en normal mode
  int16_t lift_y                = -6;     // translate_y pendant le drag
};

// Enregistre un widget (n'importe quel lv_obj_t) :
//  - draggable=true  : set_pos + overlay transparent enfant pour
//                      intercepter toutes les interactions (simple-clic
//                      relay + drag en edit mode).
//  - draggable=false : set_pos uniquement. Le widget garde son
//                      comportement YAML natif (on_press, on_long_press,
//                      etc.), il sert de slot "pinne" que le reflow
//                      ne touchera pas.
inline void attach(lv_obj_t* obj, int idx, int16_t cx, int16_t cy,
                   AttachOptions opts = {}) {
  if (obj == nullptr) return;
  if (idx < 0 || idx >= MAX_BUTTONS) return;

  g_cells[idx].x = cx;
  g_cells[idx].y = cy;
  g_buttons[idx] = obj;
  g_cell_of[idx] = idx;
  g_button_at[idx] = idx;
  g_pinned[idx]  = !opts.draggable;
  g_breathe[idx] = opts.breathe;
  g_relay_press_immediate[idx] = opts.relay_press_immediate;
  g_apply_pressed_state[idx]   = opts.apply_pressed_state;
  g_lift_y[idx]                = opts.lift_y;
  if (idx + 1 > g_count) g_count = idx + 1;
  lv_obj_set_pos(obj, cx, cy);

  // Pinne : on n'installe PAS d'overlay, on ne touche pas a CLICKABLE.
  // L'utilisateur garde on_press / on_long_press YAML natif sur le widget.
  if (!opts.draggable) {
    g_overlays[idx] = nullptr;
    return;
  }

  // Bouton non-clickable : plus jamais d'event direct. L'overlay est
  // l'unique chemin vers le bouton (via lv_obj_send_event sur simple
  // clic ou via un relay manuel d'etat LV_STATE_PRESSED).
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);

  // Overlay transparent enfant, TOUJOURS visible et clickable.
  lv_obj_t* ov = lv_obj_create(obj);
  lv_obj_remove_style_all(ov);
  lv_obj_set_size(ov, LV_PCT(100), LV_PCT(100));
  lv_obj_center(ov);
  lv_obj_set_style_bg_opa(ov, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(ov, 0, 0);
  lv_obj_set_style_pad_all(ov, 0, 0);
  lv_obj_set_style_radius(ov, 0, 0);
  lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(ov, LV_OBJ_FLAG_PRESS_LOCK);
  lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(ov, LV_OBJ_FLAG_EVENT_BUBBLE);

  lv_obj_add_event_cb(ov, overlay_event_cb, LV_EVENT_ALL,
                      reinterpret_cast<void*>(static_cast<intptr_t>(idx)));

  g_overlays[idx] = ov;
}

inline void set_cell_size(int w, int h) {
  g_cell_w = static_cast<int16_t>(w);
  g_cell_h = static_cast<int16_t>(h);
}

inline void set_reflow_ms(uint32_t ms)            { g_reflow_ms = ms; }
inline void set_breathe_amplitude(int16_t px)     { g_breathe_amplitude = px; }
inline void set_breathe_period_ms(uint32_t ms)    { g_breathe_period_ms = ms; }
inline void set_breathe_phase_step_ms(uint32_t ms){ g_breathe_phase_step_ms = ms; }
inline void set_long_press_to_edit(bool enabled)  { g_long_press_to_edit = enabled; }

}  // namespace draggable_grid


// ============================================================
// ESPHome Component wrapper
// ============================================================
namespace esphome {
namespace draggable_grid_cmpt {

struct PendingEntry {
  lv_obj_t* obj;
  int8_t    idx;
  int16_t   x;
  int16_t   y;
  ::draggable_grid::AttachOptions opts;
};

class DraggableGridComponent : public Component {
 public:
  // Surcharges :
  //  - add(obj, idx, x, y) : tout par defaut
  //  - add(obj, idx, x, y, draggable) : compat ascendante
  //  - add(obj, idx, x, y, draggable, breathe, relay_press_immediate,
  //        apply_pressed_state, lift_y) : full per-button
  void add(lv_obj_t* obj, int idx, int16_t x, int16_t y) {
    this->add_full_(obj, idx, x, y, ::draggable_grid::AttachOptions{});
  }
  void add(lv_obj_t* obj, int idx, int16_t x, int16_t y, bool draggable) {
    ::draggable_grid::AttachOptions o;
    o.draggable = draggable;
    this->add_full_(obj, idx, x, y, o);
  }
  void add(lv_obj_t* obj, int idx, int16_t x, int16_t y,
           bool draggable, bool breathe, bool relay_press_immediate,
           bool apply_pressed_state, int16_t lift_y) {
    ::draggable_grid::AttachOptions o;
    o.draggable             = draggable;
    o.breathe               = breathe;
    o.relay_press_immediate = relay_press_immediate;
    o.apply_pressed_state   = apply_pressed_state;
    o.lift_y                = lift_y;
    this->add_full_(obj, idx, x, y, o);
  }

  void set_cell_size(int w, int h) {
    this->cell_w_ = static_cast<int16_t>(w);
    this->cell_h_ = static_cast<int16_t>(h);
  }
  void set_reflow_ms(uint32_t ms)             { this->reflow_ms_ = ms; }
  void set_breathe_amplitude(int16_t px)      { this->breathe_amplitude_ = px; }
  void set_breathe_period_ms(uint32_t ms)     { this->breathe_period_ms_ = ms; }
  void set_breathe_phase_step_ms(uint32_t ms) { this->breathe_phase_step_ms_ = ms; }
  void set_long_press_to_edit(bool enabled)   { this->long_press_to_edit_ = enabled; }

  void setup() override {
    ::draggable_grid::set_cell_size(this->cell_w_, this->cell_h_);
    ::draggable_grid::set_reflow_ms(this->reflow_ms_);
    ::draggable_grid::set_breathe_amplitude(this->breathe_amplitude_);
    ::draggable_grid::set_breathe_period_ms(this->breathe_period_ms_);
    ::draggable_grid::set_breathe_phase_step_ms(this->breathe_phase_step_ms_);
    ::draggable_grid::set_long_press_to_edit(this->long_press_to_edit_);
    for (int i = 0; i < this->count_; ++i) {
      auto& e = this->entries_[i];
      if (e.obj != nullptr) {
        ::draggable_grid::attach(e.obj, e.idx, e.x, e.y, e.opts);
      }
    }
  }

  float get_setup_priority() const override {
    return setup_priority::LATE;
  }

  // Declencheurs externes : permettent d'entrer / sortir du edit mode
  // depuis un bouton non-draggable ou depuis une autre source YAML.
  void toggle_edit_mode() { ::draggable_grid::toggle_edit_mode(); }
  void set_edit_mode(bool enabled) {
    ::draggable_grid::set_edit_mode(enabled);
  }

 private:
  void add_full_(lv_obj_t* obj, int idx, int16_t x, int16_t y,
                 const ::draggable_grid::AttachOptions& o) {
    if (this->count_ >= ::draggable_grid::MAX_BUTTONS) return;
    this->entries_[this->count_++] =
        {obj, static_cast<int8_t>(idx), x, y, o};
  }

  PendingEntry entries_[::draggable_grid::MAX_BUTTONS]{};
  int8_t   count_ = 0;
  int16_t  cell_w_ = 150;
  int16_t  cell_h_ = 100;
  uint32_t reflow_ms_ = 150;
  int16_t  breathe_amplitude_ = -2;
  uint32_t breathe_period_ms_ = 1000;
  uint32_t breathe_phase_step_ms_ = 100;
  bool     long_press_to_edit_ = true;
};

// ------------------------------------------------------------
// Actions YAML (external_trigger) :
//   on_long_press:
//     - draggable_grid.toggle_edit_mode: my_grid_id
//     - draggable_grid.set_edit_mode:
//         id: my_grid_id
//         value: true
// ------------------------------------------------------------
template<typename... Ts>
class ToggleEditModeAction : public Action<Ts...> {
 public:
  explicit ToggleEditModeAction(DraggableGridComponent* parent)
      : parent_(parent) {}
  void play(Ts... /*x*/) override { this->parent_->toggle_edit_mode(); }

 private:
  DraggableGridComponent* parent_;
};

template<typename... Ts>
class SetEditModeAction : public Action<Ts...> {
 public:
  explicit SetEditModeAction(DraggableGridComponent* parent)
      : parent_(parent) {}
  TEMPLATABLE_VALUE(bool, value)
  void play(Ts... x) override {
    this->parent_->set_edit_mode(this->value_.value(x...));
  }

 private:
  DraggableGridComponent* parent_;
};

}  // namespace draggable_grid_cmpt
}  // namespace esphome
