#pragma once
//
// Native ESPHome component for LVGL draggable grid (Magnetic Push-Away).
// 100% declaratif cote YAML : aucune lambda ne doit etre ecrite par
// l'utilisateur, et le YAML des boutons reste inchange.
//
// Gestuelle :
//   - simple clic sur un bouton      -> l'event LV_EVENT_PRESSED est
//                                       relaye au bouton -> on_press se
//                                       declenche (ouvre la page).
//   - long-press sur un bouton       -> entre en "edit mode".
//     (en edit mode)
//   - drag d'un bouton               -> MAGNETIC PUSH-AWAY : le bouton
//                                       drague suit le doigt au pixel
//                                       pres (placement libre style
//                                       EEZ Studio). Les voisins qu'il
//                                       chevauche sont repousses du
//                                       Minimum Translation Vector
//                                       (axe de plus petite penetration)
//                                       en cascade : si B pousse C, C
//                                       pousse D, etc. Les boutons
//                                       pinnes sont des obstacles
//                                       immobiles. Le bouton drague
//                                       reste exactement ou il est
//                                       lache (pas de snap-to-cell).
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
//   g_overlays[idx] ne sont jamais reordonnes. Le mode magnetic ne
//   modifie que les positions pixel via lv_obj_set_pos, jamais
//   l'index ; le user_data du callback reste donc valide.
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

// Tables FIXES, indexees par identite de bouton (idx).
inline lv_obj_t* g_buttons[MAX_BUTTONS]{};   // pointeur du bouton idx
inline lv_obj_t* g_overlays[MAX_BUTTONS]{};  // pointeur de l'overlay idx
inline Cell      g_cells[MAX_BUTTONS]{};     // position INITIALE en pixels
                                             // (apres setup, le placement
                                             // libre style EEZ remplace
                                             // ces valeurs)
inline bool      g_pinned[MAX_BUTTONS]{};    // draggable=false : pas de
                                             // overlay, exclu du push,
                                             // sert d'obstacle immobile

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

// Reflow en ~150 ms : snap mais visible, proche du feel iOS.
constexpr uint32_t REFLOW_MS = 150;

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
  lv_anim_set_time(&a, REFLOW_MS);
  lv_anim_set_values(&a, 0, 1000);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

inline void kill_pos_anim(int8_t btn_idx) {
  if (btn_idx < 0 || btn_idx >= MAX_BUTTONS) return;
  lv_anim_delete(&g_move_anims[btn_idx], anim_pos_exec_cb);
}

// =============================================================
// Magnetic Push-Away (free-form drag, AABB collision response)
// =============================================================
// Inspire des moteurs physiques 2D (Box2D narrow-phase + MTV) mais
// applique au layout LVGL : le bouton drague suit le doigt au pixel
// pres (pas de snap-to-cell), et les voisins qu'il chevauche sont
// pousses du Minimum Translation Vector (axe de plus petite
// penetration). Le push se propage en cascade : si B pousse C, C
// pousse D, etc. Les boutons pinnes sont des obstacles immobiles.
//
// Pourquoi pas un repack iOS ? Le repack force tous les boutons a
// occuper des cellules pre-definies, ce qui interdit le placement
// libre style EEZ Studio. Le Magnetic Push-Away laisse l'utilisateur
// poser un bouton n'importe ou et garantit pourtant zero
// chevauchement, en ne deplaceant que strictement les voisins
// genes.
//
// Pourquoi pas un swap ? Idem : un swap force des positions
// canoniques. Ici on veut une grille "liquide", chaque bouton a sa
// position pixel propre.
//
// Cout : O(N^2) par iteration, O(K * N^2) au pire avec K cascades
// (K <= MAX_PUSH_ITER). N <= 16 -> ~256 comparaisons AABB par
// iteration, totalement negligeable. 100% PPA-friendly : on ne fait
// que des lv_obj_set_pos (translation), aucun scale.

// Renvoie true si les rectangles A et B se chevauchent (interieurs
// strictement secants : un contact aux bords ne compte pas).
inline bool aabb_overlap(int16_t ax, int16_t ay, int16_t aw, int16_t ah,
                         int16_t bx, int16_t by, int16_t bw, int16_t bh) {
  return ax < (int16_t)(bx + bw) && bx < (int16_t)(ax + aw) &&
         ay < (int16_t)(by + bh) && by < (int16_t)(ay + ah);
}

// Calcule le Minimum Translation Vector pour separer B de A. On
// teste les 4 axes possibles (push B a droite, gauche, bas, haut)
// et on retient l'axe + signe avec la penetration la plus petite.
// Resultat : *dx, *dy contient le delta a appliquer a B (un seul
// des deux est non nul -> push axis-aligned).
inline void compute_mtv(int16_t ax, int16_t ay, int16_t aw, int16_t ah,
                        int16_t bx, int16_t by, int16_t bw, int16_t bh,
                        int16_t* dx, int16_t* dy) {
  const int16_t pen_right = (int16_t)((ax + aw) - bx); // push B vers la droite
  const int16_t pen_left  = (int16_t)((bx + bw) - ax); // push B vers la gauche
  const int16_t pen_down  = (int16_t)((ay + ah) - by); // push B vers le bas
  const int16_t pen_up    = (int16_t)((by + bh) - ay); // push B vers le haut

  const int16_t x_push = (pen_right < pen_left) ?  pen_right : (int16_t)(-pen_left);
  const int16_t y_push = (pen_down  < pen_up)   ?  pen_down  : (int16_t)(-pen_up);

  const int16_t x_abs = (x_push < 0) ? (int16_t)(-x_push) : x_push;
  const int16_t y_abs = (y_push < 0) ? (int16_t)(-y_push) : y_push;

  if (x_abs <= y_abs) { *dx = x_push; *dy = 0;       }
  else                { *dx = 0;       *dy = y_push; }
}

// Recupere l'AABB courant d'un bouton (position align + taille de
// cellule globale). On ne regarde pas lv_obj_get_width/height
// pendant le drag : la taille reelle peut inclure padding/border et
// la cellule g_cell_w/h est la reference logique de collision.
inline void get_aabb(int8_t idx,
                     int16_t* x, int16_t* y, int16_t* w, int16_t* h) {
  lv_obj_t* btn = g_buttons[idx];
  *x = (int16_t)lv_obj_get_x_aligned(btn);
  *y = (int16_t)lv_obj_get_y_aligned(btn);
  *w = g_cell_w;
  *h = g_cell_h;
}

// Nombre maximum de passes de relaxation par evenement PRESSING.
// 8 passes suffisent largement pour resoudre les chaines de cascade
// jusqu'a 8 boutons aligned ; au dela on accepte un residu d'overlap
// transitoire qui sera resolu au tick suivant.
constexpr int MAGNETIC_MAX_ITER = 8;

// Resolution magnetique : apres que le bouton drague a bouge, on
// detecte les chevauchements et on pousse les non-pinnes hors de
// portee via MTV. Cascade jusqu'a stabilisation ou MAX_ITER.
//
// On pousse uniquement les boutons NON drages et NON pinnes. Le
// bouton drague reste sous le doigt ; les pinnes sont des
// obstacles fixes (si un push genererait un overlap avec un pinne,
// la passe suivante reglera le probleme en poussant sur l'autre
// axe).
inline void magnetic_resolve(int8_t dragged_idx) {
  if (dragged_idx < 0 || dragged_idx >= g_count) return;

  for (int iter = 0; iter < MAGNETIC_MAX_ITER; ++iter) {
    bool any_pushed = false;

    for (int8_t a = 0; a < g_count; ++a) {
      lv_obj_t* obj_a = g_buttons[a];
      if (obj_a == nullptr) continue;
      int16_t ax, ay, aw, ah;
      get_aabb(a, &ax, &ay, &aw, &ah);

      for (int8_t b = 0; b < g_count; ++b) {
        if (b == a) continue;
        if (b == dragged_idx) continue;  // le drague suit le doigt
        if (g_pinned[b]) continue;       // les pinnes sont immobiles

        lv_obj_t* obj_b = g_buttons[b];
        if (obj_b == nullptr) continue;
        int16_t bx, by, bw, bh;
        get_aabb(b, &bx, &by, &bw, &bh);

        if (!aabb_overlap(ax, ay, aw, ah, bx, by, bw, bh)) continue;

        int16_t dx, dy;
        compute_mtv(ax, ay, aw, ah, bx, by, bw, bh, &dx, &dy);

        // Clamp aux bornes du parent : evite qu'un push sorte le
        // bouton hors de l'ecran. Si le clamp annule le push sur
        // l'axe choisi, on tente l'autre axe pour ne pas rester
        // bloque dans un overlap.
        lv_obj_t* parent = lv_obj_get_parent(obj_b);
        const int16_t pw = (parent != nullptr) ? (int16_t)lv_obj_get_width(parent)  : INT16_MAX;
        const int16_t ph = (parent != nullptr) ? (int16_t)lv_obj_get_height(parent) : INT16_MAX;

        int16_t new_bx = (int16_t)(bx + dx);
        int16_t new_by = (int16_t)(by + dy);

        if (new_bx < 0)              new_bx = 0;
        if (new_by < 0)              new_by = 0;
        if (new_bx + bw > pw)        new_bx = (int16_t)(pw - bw);
        if (new_by + bh > ph)        new_by = (int16_t)(ph - bh);

        // Si le clamp a annule le mouvement, tente l'autre axe.
        if (new_bx == bx && new_by == by) {
          if (dx != 0) {
            // X bloque : essaie Y avec le sens donne par la position
            // relative des centres.
            const int16_t a_cy = (int16_t)(ay + ah / 2);
            const int16_t b_cy = (int16_t)(by + bh / 2);
            int16_t y_push = (b_cy >= a_cy)
                ? (int16_t)((ay + ah) - by)   // push vers le bas
                : (int16_t)(-((by + bh) - ay));// push vers le haut
            new_by = (int16_t)(by + y_push);
            if (new_by < 0) new_by = 0;
            if (new_by + bh > ph) new_by = (int16_t)(ph - bh);
          } else if (dy != 0) {
            const int16_t a_cx = (int16_t)(ax + aw / 2);
            const int16_t b_cx = (int16_t)(bx + bw / 2);
            int16_t x_push = (b_cx >= a_cx)
                ? (int16_t)((ax + aw) - bx)
                : (int16_t)(-((bx + bw) - ax));
            new_bx = (int16_t)(bx + x_push);
            if (new_bx < 0) new_bx = 0;
            if (new_bx + bw > pw) new_bx = (int16_t)(pw - bw);
          }
        }

        if (new_bx == bx && new_by == by) continue; // vraiment coince

        lv_obj_set_pos(obj_b, new_bx, new_by);
        any_pushed = true;
      }
    }

    if (!any_pushed) break;
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
  lv_anim_set_time(&a, 1000);
  lv_anim_set_playback_time(&a, 1000);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_values(&a, 0, -2);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_set_delay(&a, static_cast<uint32_t>(idx) * 100u);
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
    stop_breathe(g_buttons[i]);
  }
}

// Relance le breathing sur tous les boutons apres un drop si on est
// toujours en edit mode.
inline void resume_all_breathe() {
  for (int8_t i = 0; i < g_count; ++i) {
    if (g_pinned[i]) continue;
    start_breathe(g_buttons[i], i);
  }
}

inline void set_edit_mode(bool enabled) {
  g_edit_mode = enabled;
  for (int8_t i = 0; i < g_count; ++i) {
    lv_obj_t* btn = g_buttons[i];
    if (btn == nullptr) continue;
    if (g_pinned[i]) continue;  // les pinned ne respirent pas : pas draggables
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
// En mode magnetic, g_buttons[idx] / g_overlays[idx] / g_pinned[idx]
// sont fixes ; seules les positions pixel (lv_obj_set_pos) bougent.
inline void overlay_event_cb(lv_event_t* e) {
  const lv_event_code_t code = lv_event_get_code(e);
  const int8_t idx = static_cast<int8_t>(
      reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
  if (idx < 0 || idx >= g_count) return;
  lv_obj_t* btn = g_buttons[idx];
  if (btn == nullptr) return;

  // --- LONG_PRESSED -------------------------------------------------
  // En edit mode : sortie du edit mode (sauf si on est en plein drag).
  // En normal mode : entree en edit mode.
  if (code == LV_EVENT_LONG_PRESSED) {
    if (g_edit_mode && g_active == idx && g_moved) return;  // drag actif
    g_long_fired = true;        // annule le relay simple-click au RELEASED
    g_active = -1;
    g_moved = false;
    lv_obj_remove_state(btn, LV_STATE_PRESSED);  // propre cote visuel
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
      // PPA pendant le drag + push (gain majeur sur ESP32-P4).
      pause_all_breathe();
      // Lift visuel du bouton saisi : translate_y pure, pas de scale,
      // donc reste dans le chemin rapide PPA.
      lv_obj_move_foreground(btn);
      lv_obj_set_style_translate_y(btn, -6, LV_PART_MAIN);
      // Annule toute anim de position en cours sur les voisins : en
      // mode magnetic les voisins sont deplaces immediatement par
      // lv_obj_set_pos, pas via animate_btn_to.
      for (int8_t i = 0; i < g_count; ++i) kill_pos_anim(i);
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
      // Applique le style `pressed:` du YAML (translate_y, bg_color,...)
      lv_obj_add_state(btn, LV_STATE_PRESSED);
    }
    return;
  }

  // --- PRESSING (drag + magnetic push-away en edit mode) ------------
  if (code == LV_EVENT_PRESSING) {
    if (!g_edit_mode) return;
    if (g_active != idx) return;
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_point_t v;
    lv_indev_get_vect(indev, &v);
    if (v.x == 0 && v.y == 0) return;
    g_moved = true;
    // 1) Le bouton saisi suit le doigt au pixel pres (positionnement
    //    libre style EEZ, pas de snap-to-cell).
    int32_t nx = lv_obj_get_x_aligned(btn) + v.x;
    int32_t ny = lv_obj_get_y_aligned(btn) + v.y;
    lv_obj_set_pos(btn, nx, ny);
    // 2) Resolution magnetique : detecte les chevauchements avec les
    //    voisins non-pinnes et les pousse via MTV en cascade. Les
    //    pinnes restent immobiles, le drague reste sous le doigt.
    magnetic_resolve(idx);
    return;
  }

  // --- RELEASED / PRESS_LOST ----------------------------------------
  if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    if (g_active != idx) return;
    g_active = -1;

    // ---- NORMAL MODE : simple-click relay ---------------------------
    if (!g_edit_mode) {
      lv_obj_remove_state(btn, LV_STATE_PRESSED);
      const bool short_click =
          (code == LV_EVENT_RELEASED) && !g_moved && !g_long_fired;
      g_moved = false;
      g_long_fired = false;
      if (short_click) {
        // Relaye PRESSED/RELEASED/CLICKED au bouton pour declencher
        // on_press: et garder un etat coherent.
        lv_obj_send_event(btn, LV_EVENT_PRESSED, nullptr);
        lv_obj_send_event(btn, LV_EVENT_RELEASED, nullptr);
        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
      }
      return;
    }

    // ---- EDIT MODE : fin du drag (positionnement libre, pas de snap)
    // En mode magnetic le bouton drague reste exactement ou il a ete
    // lache : pas d'animation vers une cellule canonique. La position
    // pixel courante EST la position finale.
    g_moved = false;
    g_long_fired = false;

    // Retire le lift (-6 px) applique au PRESSED. Le breathe relance
    // ensuite reprendra a 0 et n'aura pas de saut visuel.
    lv_obj_set_style_translate_y(btn, 0, LV_PART_MAIN);

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

// Enregistre un bouton :
//  - draggable=true  : set_pos + overlay transparent enfant pour
//                      intercepter toutes les interactions (simple-clic
//                      relay + drag en edit mode).
//  - draggable=false : set_pos uniquement. Le bouton garde son
//                      comportement YAML natif (on_press, on_long_press,
//                      etc.), il sert de slot "pinne" que le reflow
//                      ne touchera pas.
inline void attach(lv_obj_t* obj, int idx, int16_t cx, int16_t cy,
                   bool draggable = true) {
  if (obj == nullptr) return;
  if (idx < 0 || idx >= MAX_BUTTONS) return;

  g_cells[idx].x = cx;
  g_cells[idx].y = cy;
  g_buttons[idx] = obj;
  g_pinned[idx]  = !draggable;
  if (idx + 1 > g_count) g_count = idx + 1;
  lv_obj_set_pos(obj, cx, cy);

  // Bouton pinne : on n'installe PAS d'overlay, on ne touche pas a
  // CLICKABLE. L'utilisateur garde on_press / on_long_press YAML natif.
  if (!draggable) {
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
  bool      draggable;
};

class DraggableGridComponent : public Component {
 public:
  void add(lv_obj_t* obj, int idx, int16_t x, int16_t y,
           bool draggable = true) {
    if (this->count_ >= ::draggable_grid::MAX_BUTTONS) return;
    this->entries_[this->count_++] =
        {obj, static_cast<int8_t>(idx), x, y, draggable};
  }

  void set_cell_size(int w, int h) {
    this->cell_w_ = static_cast<int16_t>(w);
    this->cell_h_ = static_cast<int16_t>(h);
  }

  void setup() override {
    ::draggable_grid::set_cell_size(this->cell_w_, this->cell_h_);
    for (int i = 0; i < this->count_; ++i) {
      auto& e = this->entries_[i];
      if (e.obj != nullptr) {
        ::draggable_grid::attach(e.obj, e.idx, e.x, e.y, e.draggable);
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
  PendingEntry entries_[::draggable_grid::MAX_BUTTONS]{};
  int8_t  count_ = 0;
  int16_t cell_w_ = 150;
  int16_t cell_h_ = 100;
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
