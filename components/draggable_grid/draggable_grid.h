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
//   - drag d'un bouton               -> REPACK iOS-style : a chaque
//                                       changement de cellule cible, le
//                                       bouton drague occupe la nouvelle
//                                       cellule et tous les autres
//                                       boutons (non drague + non pinnes)
//                                       sont retries puis reflowes vers
//                                       la premiere position libre dans
//                                       l'ordre geometrique haut-gauche
//                                       -> bas-droite. Exactement comme
//                                       l'iPhone reorganise sa home
//                                       screen : la grille reste pleine
//                                       et continue, les icones non
//                                       saisies decalent en cascade.
//                                       Au lacher, le bouton se pose sur
//                                       la derniere case survolee.
//   - long-press sans bouger         -> sortie du edit mode.
// - Stockage statique (inline variables) : ~300 octets, zero heap.
// - g_active / g_moved sont remis a 0/-1 en fin de chaque press.
// - L'identite d'un bouton (idx) est STABLE : g_buttons[idx] et
//   g_overlays[idx] ne sont jamais reordonnes. Les swaps ne touchent
//   g_overlays[idx] ne sont jamais reordonnes. Le repack ne touche
//   que g_cell_of[] et g_button_at[], donc le user_data du callback
//   reste valide apres un nombre quelconque d'echanges.
//   reste valide apres un nombre quelconque de reorganisations.
//

#include "esphome/core/component.h"
inline bool      g_pinned[MAX_BUTTONS]{};    // draggable=false : pas de
                                             // overlay, pas dans le reflow

// Mappings courants cellule <-> bouton. Mis a jour uniquement au swap.
// Mappings courants cellule <-> bouton. Mis a jour uniquement au repack.
inline int8_t    g_cell_of[MAX_BUTTONS]{};
inline int8_t    g_button_at[MAX_BUTTONS]{};

inline bool      g_parent_was_scrollable = false;

// Cellule actuellement occupee par le bouton saisi (mise a jour pendant
// le PRESSING). Sert a eviter de re-declencher un swap quand le doigt
// le PRESSING). Sert a eviter de re-declencher un repack quand le doigt
// reste dans la meme case.
inline int8_t    g_last_target_cell = -1;

  lv_anim_delete(&g_move_anims[btn_idx], anim_pos_exec_cb);
}

// Swap pur : le bouton drague echange sa cellule avec l'occupant de
// target_cell. Aucun autre bouton ne bouge.
// Calcule l'ordre de scan geometrique des cellules : haut-gauche ->
// bas-droite (tri par y croissant, puis x croissant). Permet de remplir
// les trous "comme une page d'iPhone" independamment de l'ordre de
// declaration YAML.
//
// Pourquoi pas un shift lineaire iOS-style ? Le shift parcourt les
// cellules dans l'ordre d'index : pour un drag horizontal sur une
// grille multi-colonnes, les cellules adjacentes en index sont aussi
// adjacentes a l'ecran, donc le shift ressemble a un swap. Mais pour
// un drag vertical, target_cell est plusieurs index plus loin que
// la cellule source, et toutes les cellules intermediaires (donc des
// boutons sur d'autres lignes/colonnes) glissent inutilement.
// Insertion sort O(N^2) sur N <= 16 : negligeable, et on l'appelle a la
// demande (pas de cache invalide a maintenir).
inline void compute_scan_order(int8_t* order, int8_t count) {
  for (int8_t i = 0; i < count; ++i) order[i] = i;
  for (int8_t i = 1; i < count; ++i) {
    const int8_t k = order[i];
    const int16_t ky = g_cells[k].y;
    const int16_t kx = g_cells[k].x;
    int8_t j = i;
    while (j > 0) {
      const int8_t p = order[j - 1];
      if (g_cells[p].y > ky ||
          (g_cells[p].y == ky && g_cells[p].x > kx)) {
        order[j] = p;
        --j;
      } else {
        break;
      }
    }
    order[j] = k;
  }
}

// iOS-style repack : le bouton drague occupe target_cell, et tous les
// autres boutons (non drague + non pinnes) sont retries puis reflowes
// vers la premiere position libre dans l'ordre geometrique. Exactement
// comme l'iPhone reorganise sa home screen quand tu deplaces une icone :
// les autres remontent / decalent en cascade pour combler les trous.
//
// Si target_cell est occupee par un bouton pinne, on l'ignore : le
// bouton pinne ne peut pas etre deplace, donc on n'echange rien.
inline void reflow_to(int8_t dragged_idx, int8_t target_cell) {
// Pourquoi pas un swap pur ? Le swap echange seulement deux boutons et
// laisse de gros trous quand on traverse plusieurs lignes. Le repack
// preserve la coherence visuelle d'une grille toujours pleine et
// continue, conformement au geste iOS.
//
// Si target_cell est occupee par un bouton pinne, on refuse : un pinne
// est intangible et ne peut etre ni pousse ni remplace.
inline void repack_to(int8_t dragged_idx, int8_t target_cell) {
  if (target_cell == g_last_target_cell) return;
  if (target_cell < 0) return;
  g_last_target_cell = target_cell;

  const int8_t src_cell = g_cell_of[dragged_idx];
  if (src_cell == target_cell) return;
  if (dragged_idx < 0 || dragged_idx >= g_count) return;

  const int8_t target_occupant = g_button_at[target_cell];
  if (target_occupant >= 0 && g_pinned[target_occupant]) return;

  if (g_cell_of[dragged_idx] == target_cell) {
    g_last_target_cell = target_cell;
    return;
  }

  g_last_target_cell = target_cell;

  // 1) Collecte des "autres" (non drague, non pinne) dans l'ordre
  //    geometrique de leur cellule actuelle. C'est cet ordre qui sera
  //    preserve lors du reflow, comme iOS qui maintient le rang relatif
  //    des icones non-saisies.
  int8_t scan[MAX_BUTTONS];
  compute_scan_order(scan, g_count);

  int8_t others[MAX_BUTTONS];
  int8_t other_count = 0;
  for (int8_t s = 0; s < g_count; ++s) {
    const int8_t cell = scan[s];
    const int8_t b = g_button_at[cell];
    if (b < 0) continue;
    if (b == dragged_idx) continue;
    if (g_pinned[b]) continue;
    others[other_count++] = b;
  }

  // 2) Liberation des cellules non-pinnees. Les pinnes restent ancres
  //    a leur cellule et sont intouchables par le repack.
  for (int8_t cell = 0; cell < g_count; ++cell) {
    const int8_t b = g_button_at[cell];
    if (b >= 0 && g_pinned[b]) continue;
    g_button_at[cell] = -1;
  }

  // 3) Place le bouton drague a la cellule cible (deja verifiee non
  //    pinnee plus haut).
  g_button_at[target_cell] = dragged_idx;
  g_cell_of[dragged_idx]   = target_cell;

  if (target_occupant >= 0 && target_occupant != dragged_idx) {
    g_button_at[src_cell]      = target_occupant;
    g_cell_of[target_occupant] = src_cell;
    animate_btn_to(target_occupant,
                   g_cells[src_cell].x, g_cells[src_cell].y);
  } else {
    g_button_at[src_cell] = -1;
  // 4) Reflow : chaque "autre" prend la premiere cellule libre dans
  //    l'ordre geometrique. Animation ease-out vers la nouvelle case.
  int8_t scan_pos = 0;
  for (int8_t i = 0; i < other_count; ++i) {
    while (scan_pos < g_count) {
      const int8_t cell = scan[scan_pos];
      if (g_button_at[cell] < 0) break;
      ++scan_pos;
    }
    if (scan_pos >= g_count) break;  // garde-fou : N cellules == N boutons
    const int8_t cell = scan[scan_pos];
    const int8_t b    = others[i];
    g_button_at[cell] = b;
    g_cell_of[b]      = cell;
    animate_btn_to(b, g_cells[cell].x, g_cells[cell].y);
  }
}

// d'entree pour toutes les interactions utilisateur.
//
// user_data = identite STABLE du bouton (idx d'origine a l'attach).
// Apres un swap, g_buttons[idx] et g_overlays[idx] ne changent PAS ;
// Apres un repack, g_buttons[idx] et g_overlays[idx] ne changent PAS ;
// seul g_cell_of[idx] est mis a jour.
inline void overlay_event_cb(lv_event_t* e) {
  const lv_event_code_t code = lv_event_get_code(e);
    int32_t cy = ny + g_cell_h / 2;
    int8_t  target = nearest_cell(cx, cy);
    if (target != g_last_target_cell) {
      reflow_to(idx, target);
      repack_to(idx, target);
    }
    return;
  }
    g_last_target_cell = -1;

    // Le bouton saisi se pose anime sur la case finale (celle deja
    // inscrite dans g_cell_of par le dernier reflow_to).
    // inscrite dans g_cell_of par le dernier repack_to).
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
  g_cell_of[idx] = idx;
  g_button_at[idx] = idx;
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
